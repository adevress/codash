
/* Copyright (c) 2012, EPFL/Blue Brain Project
 *                     Daniel Nachbaur <daniel.nachbaur@epfl.ch>
 *
 * This file is part of CoDASH <https://github.com/BlueBrain/codash>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3.0 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "receiver.h"
#include "detail/communicator.h"
#include "detail/types.h"

#include <co/connectionDescription.h>
#include <co/customOCommand.h>
#include <co/global.h>
#include <co/objectMap.h>

#include <lunchbox/mtQueue.h>

#include <boost/bind.hpp>
#include <boost/foreach.hpp>


namespace codash
{

typedef stde::hash_map< std::string, ReceiverPtr > Receivers;
typedef Receivers::iterator ReceiversIter;
typedef Receivers::const_iterator ReceiversCIter;
static Receivers _receivers;

namespace detail
{

typedef boost::function< void() > WorkFunc;

class Receiver : public Communicator
{
public:
    Receiver()
        : Communicator( co::ConnectionDescriptionPtr( ))
        , _proxyNode()
        , _mapQueue()
        , _nodes()
        , _queuedVersions()
        , _objectMapVersion( co::VERSION_FIRST )
    {
        _init();
    }

    explicit Receiver( co::LocalNodePtr localNode )
        : Communicator( localNode )
        , _proxyNode()
        , _mapQueue()
        , _nodes()
        , _queuedVersions()
        , _objectMapVersion( co::VERSION_FIRST )
    {
        _init();
    }

    ~Receiver()
    {
        _objectMap->clear();
        _dashNodes.clear();
        disconnect();
    }

    bool connect( co::ConnectionDescriptionPtr conn )
    {
        if( !conn || isConnected( ))
            return false;

        _proxyNode = new co::Node;
        _proxyNode->addConnectionDescription( conn );
        if( !_localNode->connect( _proxyNode ))
            return false;

        return _connect();
    }

    bool connect( const co::NodeID& nodeID )
    {
        if( isConnected( ))
            return false;

        _proxyNode = _localNode->connect( nodeID );
        if( !_proxyNode )
            return false;

        return _connect();
    }

    bool disconnect()
    {
        if( !isConnected() || !_localNode->disconnect( _proxyNode ))
            return false;

        _proxyNode = co::NodePtr();
        return true;
    }

    bool isConnected() const
    {
        if( !_proxyNode )
            return false;
        return _proxyNode->isConnected();
    }

    co::ConstConnectionDescriptionPtr getConnection()
    {
        return _proxyNode && _proxyNode->isConnected()
                    ? _proxyNode->getConnection()->getDescription()
                    : co::ConstConnectionDescriptionPtr();
    }

    const dash::Nodes& getNodes() const
    {
        if( !_dashNodes.empty( ))
            return _dashNodes;

        BOOST_FOREACH( const UUID& id, _nodes )
        {
            Node* node = static_cast< Node* >( _objectMap->map( id ));
            dash::NodePtr dashNode = node->getValue();
            _dashNodes.push_back( dashNode );
        }

        return _dashNodes;
    }

    virtual bool syncOne()
    {
        uint128_t version;
        while( !_queuedVersions.timedPop( co::Global::getKeepaliveTimeout(),
                                          version ))
        {
            if( !isConnected( ))
            {
                LBWARN << "Lost connection to sender while waiting for new data"
                       << std::endl;
                return false;
            }
            else
                LBWARN << "Got timeout while waiting for new data" << std::endl;
        }

        Communicator::sync( version );
        _objectMap->sync( _objectMapVersion );

        return true;
    }

    virtual void serialize( co::DataOStream& os, const uint64_t dirtyBits )
    {
        LBDONTCALL
        Communicator::serialize( os, dirtyBits );
    }

    virtual void deserialize( co::DataIStream& is, const uint64_t dirtyBits )
    {
        if( dirtyBits & DIRTY_NODES )
        {
            _nodes.clear();
            is >> _nodes;
            _dashNodes.clear();
        }
        if( dirtyBits & DIRTY_OBJECTMAP )
        {
            co::ObjectVersion ov;
            is >> ov;
            _objectMapVersion = ov.version;
            if( !_objectMap->isAttached( ))
            {
                _mapQueue.push_back( boost::bind( &co::LocalNode::mapObject,
                                                  _localNode.get(), _objectMap,
                                                  ov ));
            }
        }

        Communicator::deserialize( is, dirtyBits );
    }

    virtual void notifyNewHeadVersion( const uint128_t& version )
    {
        _queuedVersions.push( version );
        std::for_each( _handlers.begin(), _handlers.end(),
                    boost::bind( &VersionHandlers::value_type::operator(), _1));

        Communicator::notifyNewHeadVersion( version );
    }

    void registerNewVersionHandler( const VersionHandler& func )
    {
        LBASSERT( !isConnected( ));
        _handlers.push_back( func );
    }

    virtual uint64_t getMaxVersions() const
    {
        return 50;
    }

private:
    void _init()
    {
        _localNode->registerPushHandler( _groupID,
                   boost::bind( &Receiver::_handleInit, this, _1, _2, _3, _4 ));
    }

    bool _connect()
    {
        _proxyNode->send( _initCmd );
        if( !_initialized.timedWaitEQ( true, co::Global::getKeepaliveTimeout()))
            return false;
        _processMappings();
        _objectMapVersion = _objectMap->getVersion();
        return true;
    }

    void _handleInit( const uint128_t& groupID, const uint128_t& typeID,
                      const UUID& objectID, co::DataIStream& istream )
    {
        if( groupID != _groupID || typeID != _typeInit )
            return;

        deserialize( istream, co::Serializable::DIRTY_ALL );
        _mapQueue.push_back( boost::bind( &co::LocalNode::mapObject,
                                          _localNode.get(), this, objectID,
                                          co::VERSION_NONE ));
        _initialized = true;
    }

    void _processMappings()
    {
        BOOST_FOREACH( const WorkFunc& func, _mapQueue )
        {
            func();
        }
        _mapQueue.clear();
    }

    co::NodePtr _proxyNode;
    std::deque< WorkFunc > _mapQueue;
    IDSet _nodes;
    mutable dash::Nodes _dashNodes;
    lunchbox::MTQueue< uint128_t > _queuedVersions;
    uint128_t _objectMapVersion;
    lunchbox::Monitor<bool> _initialized;
    typedef std::vector< VersionHandler > VersionHandlers;
    VersionHandlers _handlers;
};
}

Receiver::Receiver()
    : _impl( new detail::Receiver )
{
}

Receiver::Receiver( co::LocalNodePtr localNode )
    : _impl( new detail::Receiver( localNode ))
{
}

Receiver::~Receiver()
{
    delete _impl;
}

ReceiverPtr Receiver::create( const std::string& identifier,
                              co::LocalNodePtr localNode )
{
    ReceiversCIter i = _receivers.find( identifier );
    if( i != _receivers.end( ))
        return i->second;
    ReceiverPtr receiver = localNode ? new Receiver( localNode ) :
                                       new Receiver;
    _receivers[identifier] = receiver;
    return receiver;
}

void Receiver::destroy( const std::string& identifier )
{
    ReceiversIter i = _receivers.find( identifier );
    if( i != _receivers.end( ))
        _receivers.erase( i );
}

void Receiver::destroy( ReceiverPtr receiver )
{
    for( ReceiversIter i = _receivers.begin(); i != _receivers.end(); ++i )
    {
        if( i->second == receiver )
        {
            _receivers.erase( i );
            break;
        }
    }
}

co::ConstLocalNodePtr Receiver::getNode() const
{
    return _impl->getNode();
}

co::Zeroconf Receiver::getZeroconf()
{
    return _impl->getZeroconf();
}

bool Receiver::connect( co::ConnectionDescriptionPtr conn )
{
    return _impl->connect( conn );
}

bool Receiver::connect( const co::NodeID& nodeID )
{
    return _impl->connect( nodeID );
}

bool Receiver::disconnect()
{
    return _impl->disconnect();
}

bool Receiver::isConnected() const
{
    return _impl->isConnected();
}

co::ConstConnectionDescriptionPtr Receiver::getConnection() const
{
    return _impl->getConnection();
}

dash::Context& Receiver::getContext()
{
    return _impl->getContext();
}

const dash::Nodes& Receiver::getNodes() const
{
    return _impl->getNodes();
}

bool Receiver::sync()
{
    return _impl->syncOne();
}

void Receiver::registerNewVersionHandler( const VersionHandler& func )
{
    return _impl->registerNewVersionHandler( func );
}

}
