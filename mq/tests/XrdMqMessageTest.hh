//------------------------------------------------------------------------------
//! @file XrdMqMessageTest.hh
//! @author Elvin Sindrilaru <esindril@cern.ch>
//! @brief Class containing unit test for the XrdMqMessage class
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016 CERN/Switzerland                                  *
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

#ifndef __EOSMQTEST_XRDMQMESSAGE_HH__
#define __EOSMQTEST_XRDMQMESSAGE_HH__

#include "common/CppUnitMacros.h"
#include "TestEnv.hh"

//! Ugly hack to expose the private functions for testing
#define protected public
#define private   public
#include "mq/XrdMqMessage.hh"
#undef protected
#undef private

//------------------------------------------------------------------------------
//! Class XrdMqMessageTest
//------------------------------------------------------------------------------
class XrdMqMessageTest: public CppUnit::TestCase
{
  CPPUNIT_TEST_SUITE(XrdMqMessageTest);
    CPPUNIT_TEST(Base64Test);
    CPPUNIT_TEST(CipherTest);
    CPPUNIT_TEST(RSATest);
  CPPUNIT_TEST_SUITE_END();

 public:

  //----------------------------------------------------------------------------
  //! setUp function
  //----------------------------------------------------------------------------
  void setUp(void);

  //----------------------------------------------------------------------------
  //! tearDown function
  //----------------------------------------------------------------------------
  void tearDown(void);

 protected:

  //----------------------------------------------------------------------------
  //! Base64 test
  //----------------------------------------------------------------------------
  void Base64Test();

  //----------------------------------------------------------------------------
  //! Cipher encoding and decoding test
  //----------------------------------------------------------------------------
  void CipherTest();

  //----------------------------------------------------------------------------
  //! RSA encoding and decoding test
  //----------------------------------------------------------------------------
  void RSATest();

 private:

  //----------------------------------------------------------------------------
  //! Generate random data
  //!
  //! @param data generated data - must be already allocated
  //! @param length length of the data
  //----------------------------------------------------------------------------
  void GenerateRandomData(char* data, ssize_t length);

  eos::mq::test::TestEnv* mEnv; ///< test environment object
};

#endif // __EOSMQTEST_XRDMQMESSAGE_HH__
