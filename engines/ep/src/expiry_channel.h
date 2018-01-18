/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Teligent; Author: Alexander Petrossian (PAF)
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

#ifndef SRC_EXPIRY_CHANNEL_H_
#define SRC_EXPIRY_CHANNEL_H_ 1

#include "config.h"
#include "stored-value.h"

#include <string>

/**
 * @brief  Expiry Channel using UDP to
 * transport expiration data (now only send without waiting for reply).
 * @author Alexander Petrossian, copy/pasted from TPTFChannelUDP by Egor Seredin
 * @date   2016-02-29, Thailand
 */
class ExpiryChannel {
public:
	ExpiryChannel();
	virtual ~ExpiryChannel();
	
	/**
	 * Open UDP socket
	 * @param dstAddr address to connect
	 * @param dstPort port to connect
	 * @return true on success, false on failure
	 */ 
	bool open(const std::string& dstAddr,
			 const int dstPort);
	
	/**
	 * Send expiration info
	 * @param v Stored value to send
	 */
	void sendNotification(const std::string& name, const StoredValue* v);

	/**
	 * Close channel, cleanup
	 */
	void close();
	
	const bool isConnected() const;
	
private:
	int mSocket;
};

#endif  // SRC_EXPIRY_CHANNEL_H_
