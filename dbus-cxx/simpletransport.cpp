/***************************************************************************
 *   Copyright (C) 2020 by Robert Middleton                                *
 *   robert.middleton@rm5248.com                                           *
 *                                                                         *
 *   This file is part of the dbus-cxx library.                            *
 *                                                                         *
 *   The dbus-cxx library is free software; you can redistribute it and/or *
 *   modify it under the terms of the GNU General Public License           *
 *   version 3 as published by the Free Software Foundation.               *
 *                                                                         *
 *   The dbus-cxx library is distributed in the hope that it will be       *
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty   *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU   *
 *   General Public License for more details.                              *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this software. If not see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
#include "simpletransport.h"

#include <dbus-cxx-private.h>
#include "demarshaling.h"
#include "message.h"
#include "utility.h"

#include <cstring>
#include <memory>
#include <unistd.h>
#include <sys/ioctl.h>

using DBus::priv::SimpleTransport;

enum class ReadingState {
    FirstHeaderPart,
    SecondHeaderPart,
    Body
};

class SimpleTransport::priv_data{
public:
    priv_data( int fd ):
        m_fd( fd ),
        m_readingState( ReadingState::FirstHeaderPart ),
        m_receiveBuffer( nullptr ),
        m_receiveBufferLocation( 0 ),
        m_receiveBufferSize( 512 ),
        m_headerLeftToRead( 0 )
    {}

    int m_fd;
    bool m_ok;
    ReadingState m_readingState;
    std::vector<uint8_t> m_sendBuffer;
    uint8_t* m_receiveBuffer;
    uint32_t m_receiveBufferLocation;
    uint32_t m_receiveBufferSize;
    uint32_t m_bodyLeftToRead;
    uint32_t m_headerLeftToRead;
};

SimpleTransport::SimpleTransport( int fd, bool initialize ) :
    m_priv( std::make_unique<priv_data>( fd ) ){
    uint8_t nulbyte = 0;

    if( initialize ){
        if( ::write( m_priv->m_fd, &nulbyte, 1 ) < 0 ){
            int my_errno = errno;
            std::string errmsg = strerror( errno );
            SIMPLELOGGER_DEBUG("dbus.SimpleTransport", "Unable to write nul byte: " + errmsg );
            m_priv->m_ok = false;
            close( m_priv->m_fd );
            errno = my_errno;
            return;
        }
    }

    m_priv->m_receiveBuffer = new uint8_t[ m_priv->m_receiveBufferSize ];
    m_priv->m_ok = true;
}

SimpleTransport::~SimpleTransport(){
    close( m_priv->m_fd );
    delete[] m_priv->m_receiveBuffer;
}


std::shared_ptr<SimpleTransport> SimpleTransport::create( int fd, bool initialize ){
    return std::shared_ptr<SimpleTransport>( new SimpleTransport( fd, initialize ) );
}

ssize_t SimpleTransport::writeMessage( std::shared_ptr<const Message> message, uint32_t serial ){
    std::ostringstream debug_str;
    m_priv->m_sendBuffer.clear();
    if( !message->serialize_to_vector( &m_priv->m_sendBuffer, serial ) ){
        return 0;
    }

    debug_str << "Going to send the following bytes: " << std::endl;
    DBus::hexdump( &m_priv->m_sendBuffer, &debug_str );
    SIMPLELOGGER_TRACE( "dbus.SimpleTransport", debug_str.str() );

    ssize_t bytesWritten = ::write( m_priv->m_fd, m_priv->m_sendBuffer.data(), m_priv->m_sendBuffer.size() );
    if( bytesWritten < 0 ){
        int my_errno = errno;
        std::string errmsg = strerror( errno );
        SIMPLELOGGER_DEBUG("dbus.SimpleTransport", "Unable to send message: " + errmsg );
        errno = my_errno;
    }

    return bytesWritten;
}

std::shared_ptr<DBus::Message> SimpleTransport::readMessage(){
    ssize_t bytesRead;
    std::shared_ptr<Message> retmsg;

    if( m_priv->m_readingState == ReadingState::FirstHeaderPart ){
        /*
         * First part of the reading: read the first 16 bytes, which include the
         * important data(including the size of the variable-length array)
         */
        bytesRead = ::read( m_priv->m_fd,
                            m_priv->m_receiveBuffer + m_priv->m_receiveBufferLocation,
                            16 - m_priv->m_receiveBufferLocation );
        if( bytesRead < 0 ){
            return std::shared_ptr<DBus::Message>();
        }
        if( bytesRead == 0 ){
            // End of the stream
            SIMPLELOGGER_TRACE( "dbus.SimpleTransport", "End of stream: closing transport" );
            m_priv->m_ok = false;
            return std::shared_ptr<DBus::Message>();
        }
        m_priv->m_receiveBufferLocation += bytesRead;

        if( m_priv->m_receiveBufferLocation == 16 ){
            m_priv->m_readingState = ReadingState::SecondHeaderPart;
        }
    }

    if( m_priv->m_readingState == ReadingState::SecondHeaderPart ){
        /*
         * Next part of the reading: read the variable-sized array in the header
         */
        if( m_priv->m_headerLeftToRead == 0 ){
            Demarshaling m( m_priv->m_receiveBuffer, 16, Endianess::Big );
            uint32_t totalMessageSize;
            uint32_t headerArraySize;
            int bytesToAdd;

            if( m.demarshal_uint8_t() == 'l' ){
                m.setEndianess( Endianess::Little );
            }

            m.setDataOffset( 4 );
            m_priv->m_bodyLeftToRead =  m.demarshal_uint32_t();
            m.setDataOffset( 12 );
            headerArraySize = m.demarshal_uint32_t();

            // Make sure that the header would align to a multiple of 8
            bytesToAdd = 8 - ((16 + headerArraySize) % 8);
            if( bytesToAdd != 8 ){
                headerArraySize += bytesToAdd;
            }
           m_priv-> m_headerLeftToRead = headerArraySize;

            totalMessageSize = ( 12 /* Basic header size */
                                 + m_priv->m_headerLeftToRead
                                 + m_priv->m_bodyLeftToRead
                                 + 12 /* Extra padding */ );
            if( m_priv->m_receiveBufferSize < totalMessageSize ){
                m_priv->m_receiveBufferSize = totalMessageSize;
                uint8_t* newbuffer = new uint8_t[ m_priv->m_receiveBufferSize ];
                std::memcpy( newbuffer, m_priv->m_receiveBuffer, m_priv->m_receiveBufferLocation );
                delete[] m_priv->m_receiveBuffer;
                m_priv->m_receiveBuffer = newbuffer;
            }
        }

        /*
         * Now that we know how big the variable-sized array is and the body size,
         * try to read that number of bytes(to keep the number of read() calls down)
         */
        bytesRead = ::read( m_priv->m_fd,
                            m_priv->m_receiveBuffer + m_priv->m_receiveBufferLocation,
                            m_priv->m_headerLeftToRead + m_priv->m_bodyLeftToRead );

        if( bytesRead < 0 ){
            return std::shared_ptr<DBus::Message>();
        }
        m_priv->m_receiveBufferLocation += bytesRead;
        if( m_priv->m_headerLeftToRead - bytesRead <= 0 ){
            bytesRead -= m_priv->m_headerLeftToRead;
            m_priv->m_headerLeftToRead = 0;
        }
        m_priv->m_bodyLeftToRead -= bytesRead;

        if( m_priv->m_headerLeftToRead == 0 ){
            // We have at least full header at this point
            m_priv->m_readingState = ReadingState::Body;
        }
    }

    if( m_priv->m_readingState == ReadingState::Body ){
        if( m_priv->m_bodyLeftToRead ){
            bytesRead = ::read( m_priv->m_fd,
                                m_priv->m_receiveBuffer + m_priv->m_receiveBufferLocation,
                                m_priv->m_bodyLeftToRead );
            if( bytesRead < 0 ){
                return std::shared_ptr<DBus::Message>();
            }
            m_priv->m_bodyLeftToRead -= bytesRead;
        }

        if( m_priv->m_bodyLeftToRead == 0 ) {
            // We have a full message at this point!
            std::ostringstream debug_str;
            debug_str << "Going to create a message from the following data: " << std::endl;
            DBus::hexdump( m_priv->m_receiveBuffer, m_priv->m_receiveBufferLocation, &debug_str );
            SIMPLELOGGER_TRACE( "dbus.SimpleTransport", debug_str.str() );

            retmsg = Message::create_from_data( m_priv->m_receiveBuffer, m_priv->m_receiveBufferLocation );

            m_priv->m_receiveBufferLocation = 0;
            m_priv->m_readingState = ReadingState::FirstHeaderPart;
            m_priv->m_headerLeftToRead = 0;
        }
    }

    return retmsg;
}

bool SimpleTransport::is_valid() const {
    return m_priv->m_ok;
}

int SimpleTransport::fd() const {
    return m_priv->m_fd;
}
