/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   Data Buffer
//------------------------------------------------------------------------------

#ifndef EOS_NS_BUFFER_HH
#define EOS_NS_BUFFER_HH

#include <cstring>
#include <vector>
#include <stdint.h>
#include <zlib.h>

#include "namespace/MDException.hh"

namespace eos
{
  //----------------------------------------------------------------------------
  //! Data Buffer
  //----------------------------------------------------------------------------
  class Buffer: public std::vector<char>
  {
    public:
      //------------------------------------------------------------------------
      //! Constructor
      //------------------------------------------------------------------------
      Buffer( unsigned size = 512 )
      {
	reserve( size );
      }

      //------------------------------------------------------------------------
      //! Destructor
      //------------------------------------------------------------------------
      virtual ~Buffer() {}

      //------------------------------------------------------------------------
      //! Copy constructor
      //------------------------------------------------------------------------
      Buffer( const Buffer &other )
      {
	*this = other;
      };

      //------------------------------------------------------------------------
      //! Assignment operator
      //------------------------------------------------------------------------
      Buffer &operator = ( const Buffer &other )
      {
	resize( other.getSize() );
	(void) memcpy( getDataPtr(), other.getDataPtr(), other.getSize() );
	return *this;
      };

      //------------------------------------------------------------------------
      //! Get data pointer
      //------------------------------------------------------------------------
      char *getDataPtr()
      {
	return &operator[]( 0 );
      }

      //------------------------------------------------------------------------
      //! Get data pointer
      //------------------------------------------------------------------------
      const char *getDataPtr() const
      {
	return &operator[]( 0 );
      }

      //------------------------------------------------------------------------
      //! Get data padded (if we read over the size we get 0 as response)
      //------------------------------------------------------------------------
      const char getDataPadded(size_t i) const
      {
	if (i < size())
	  return (operator[](i));
	return 0;
      }

      //------------------------------------------------------------------------
      //! Get size
      //------------------------------------------------------------------------
      size_t getSize() const
      {
	return size();
      }

      //------------------------------------------------------------------------
      //! Add data
      //------------------------------------------------------------------------
      void putData( const void *ptr, size_t dataSize )
      {
	size_t currSize = size();
	resize( currSize + dataSize );
	(void) memcpy( &operator[](currSize), ptr, dataSize );
      }

      //------------------------------------------------------------------------
      //! Add data
      //------------------------------------------------------------------------
      uint16_t grabData( uint16_t offset, void *ptr, size_t dataSize ) const
	throw( MDException )
      {
	if( offset+dataSize > getSize() )
	{
	  MDException e( EINVAL );
	  e.getMessage() << "Not enough data to fulfil the request";
	  throw e;
	}
	(void) memcpy( ptr, &operator[](offset), dataSize );
	return offset+dataSize;
      }

    //--------------------------------------------------------------------------
    //! Copy specified amount of data from buffer to new destination
    //!
    //! @param buffer std::string holding source binary blob
    //! @param offset offset in the buffer from where we start copying
    //! @param size amount of data that we copy
    //! @param dest_ptr destination where data is copied
    //!
    //! @return new offset in the original string
    //--------------------------------------------------------------------------
    static uint64_t
    grabData(const std::string& buffer, uint64_t offset, void* dest_ptr, uint64_t size)
    {
      const char* src_ptr = buffer.data() + offset;
      (void) memcpy(dest_ptr, src_ptr, size);
      return offset + size;
    }

    //------------------------------------------------------------------------
    //! Calculate the CRC32 checksum
    //------------------------------------------------------------------------
    uint32_t getCRC32() const
    {
      return crc32( crc32( 0L, Z_NULL, 0 ),
		    (const Bytef*)getDataPtr(), size() );
    }
  protected:
  };
}

#endif // EOS_NS_BUFFER_HH
