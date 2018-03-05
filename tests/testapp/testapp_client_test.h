/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#pragma once

#include  <algorithm>

#include "testapp.h"

/**
 * Test fixture for testapp tests; parameterised on the TransportProtocol (IPv4,
 * Ipv6); (Plain, SSL).
 */
class TestappClientTest
    : public TestappTest,
      public ::testing::WithParamInterface<TransportProtocols> {
protected:
    MemcachedConnection& getConnection() override;
};

enum class XattrSupport { Yes, No };
std::ostream& operator<<(std::ostream& os, const XattrSupport& xattrSupport);
std::string to_string(const XattrSupport& xattrSupport);

/**
 * Test fixture for Extended Attribute (XATTR) tests.
 * Parameterized on:
 * - TransportProtocol (IPv4, Ipv6); (Plain, SSL);
 * - XATTR On/Off
 * - Client JSON On/Off.
 * - Client SNAPPY On/Off.
 */
class TestappXattrClientTest : public TestappTest,
                               public ::testing::WithParamInterface<
                                       ::testing::tuple<TransportProtocols,
                                                        XattrSupport,
                                                        ClientJSONSupport,
                                                        ClientSnappySupport>> {
protected:
    TestappXattrClientTest()
        : xattrOperationStatus(PROTOCOL_BINARY_RESPONSE_SUCCESS) {
    }

    void SetUp() override;

    MemcachedConnection& getConnection() override;

    BinprotSubdocResponse getXattr(const std::string& path,
                                   bool deleted = false);
    void createXattr(const std::string& path,
                     const std::string& value,
                     bool macro = false);

    ClientJSONSupport hasJSONSupport() const override;

    virtual ClientSnappySupport hasSnappySupport() const;

    // What response datatype do we expect for documents which are JSON?
    // Will be JSON only if the client successfully negotiated JSON feature.
    cb::mcbp::Datatype expectedJSONDatatype() const;

    /**
     * Helper function to check datatype is what we expect for this test config;
     * and if datatype says JSON, validate the value /is/ JSON.
     */
    static ::testing::AssertionResult hasCorrectDatatype(
            const Document& doc, cb::mcbp::Datatype expectedType);

    static ::testing::AssertionResult hasCorrectDatatype(
            cb::mcbp::Datatype expectedType,
            cb::mcbp::Datatype actualType,
            cb::const_char_buffer value);

    void setClusterSessionToken(uint64_t new_value);

protected:
    Document document;
    protocol_binary_response_status xattrOperationStatus;
};

struct PrintToStringCombinedName {
    std::string operator()(const ::testing::TestParamInfo<
                           ::testing::tuple<TransportProtocols,
                                            XattrSupport,
                                            ClientJSONSupport,
                                            ClientSnappySupport>>& info) const;
};
