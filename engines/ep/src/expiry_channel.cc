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

#include "expiry_channel.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>

#include <sstream>

#include <cJSON.h>

static const size_t MAX_PACKET_SIZE=65000;


ExpiryChannel::ExpiryChannel(): mSocket(-1) {
}

ExpiryChannel::~ExpiryChannel() {
	close();
}

bool ExpiryChannel::open(const std::string& dstAddr, const int dstPort) {
	LOG(EXTENSION_LOG_INFO, "%s: open(%s:%d)", __func__, dstAddr.c_str(), dstPort);
	close();
	if(dstAddr.empty() || !dstPort) {
		return false;
	}
	mSocket = socket(PF_INET, SOCK_DGRAM, 0);
	if(mSocket < 0) {
		LOG(EXTENSION_LOG_WARNING, "%s: open(%s:%d): failed to open UDP socket errno[%d]", __func__, dstAddr.c_str(), dstPort, errno);
		return false;
	}
	
	{
		struct hostent htmp, *hp(NULL);
		char aux_data[4*0x400];
		int herr(0);

		memset(&htmp, 0, sizeof (htmp));
		
		// Get hostname
		const int res = gethostbyname_r(dstAddr.c_str(),
			&htmp, aux_data, sizeof aux_data, &hp, &herr);
		
		if(res || !hp) {
			LOG(EXTENSION_LOG_WARNING, "%s: gethostname_r failed: res=%d, herr[%d]",
				__func__, res, herr);
			return false;
		}
		
		if(!(hp->h_addr_list && hp->h_addr_list[0])) {
			LOG(EXTENSION_LOG_WARNING, "%s: gethostname_r returned incorrect data",
				__func__);
			return false;
		}

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		assert(hp->h_length <= sizeof addr.sin_addr);
		memcpy((char *) &addr.sin_addr, (char *) hp->h_addr, hp->h_length);
		addr.sin_port = htons(dstPort);
		
		int rc;
		if((rc = connect(mSocket, (sockaddr*)&addr, sizeof(addr))) < 0) {
			LOG(EXTENSION_LOG_WARNING, "%s: conn(%d,%d)=%d. errno[%d]",
				__func__, addr.sin_addr.s_addr, addr.sin_port, rc, errno);
			close();
			return false;
		}
	}
	
	int mtu = IP_PMTUDISC_DONT;
	socklen_t socklen = sizeof(mtu);
	setsockopt(mSocket, IPPROTO_IP, IP_MTU_DISCOVER, &mtu, socklen);
	
	return true;
}

void ExpiryChannel::sendNotification(const std::string& name, const StoredValue* v) {
	if(!isConnected()) {
//		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s], but there is no connection (not configured? failed to open?), bailing out...", __func__, name.c_str(), v->getKey().c_str());
		return;
	}
	if(!v) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s]: called without StoredValue, bailing out...", __func__, name.c_str());
		return;
	}
/*
{
  "bucket": "<STYPE>",
  "id": "<SID>",
  "expiry": "<EXPIRES>",
  "cas": "<CAS value (version)>",
  "flags": <flags>,
  "body": <PAYLOAD in BOX format>  // datatype==0? "string", datatype==1(JSON) {...}
}
*/
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "bucket", name.c_str());
	cJSON_AddStringToObject(root, "id", v->getKey().c_str());
	cJSON_AddNumberToObject(root, "expiry", v->getExptime());
	//int64->double possibly loses precision, fatal for 'cas', not doing that for now, not really needed // cJSON_AddNumberToObject(root, "cas", v->getCas());
	cJSON_AddNumberToObject(root, "flags", v->getFlags());

	const value_t& d = v->getValue();
	uint8_t t = d->getDataType();
	const std::string sbody(d->to_s());
	switch(t) {
		case PROTOCOL_BINARY_DATATYPE_JSON: {
			cJSON* jbody = cJSON_Parse(sbody.c_str());
			if (jbody)
				cJSON_AddItemToObject(root, "body", jbody); // assumes responsibility
			else
				LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: reported its type as JSON but can not parse it, bailing out...", __func__, name.c_str(), v->getKey().c_str());
			break;
		}
		case PROTOCOL_BINARY_RAW_BYTES: {
			cJSON_AddStringToObject(root, "body", sbody.c_str());
			break;
		}
		default:
			LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: can not handle its type[%d] (it's neither RAW=0 nor JSON=1), sending without body", __func__, name.c_str(), v->getKey().c_str(), t);
			break;
	}

	char* json_cstr = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_cstr) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: failed to serialize to json. Had good type[%u] (RAW=0, JSON=1), bailing out...", __func__, name.c_str(), v->getKey().c_str(), (unsigned)t);
		return;
	}
	size_t json_length = strlen(json_cstr);
	if (json_length > MAX_PACKET_SIZE) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: serialized to json_length[%zu], which is more than MAX_PACKET_SIZE[%zu], bailing out...", __func__, name.c_str(), v->getKey().c_str(), json_length, MAX_PACKET_SIZE);
		cJSON_Free(json_cstr);
		return;
	}

	// here discovered errno==ECONNREFUSED to be returned if PREVIOUS message to that destination was not delivered
	// (came ICMP with error as a reply to PREVIOUS message)
	// official way to deal with it is to retry that case
	// here discovered errno==EINTR also for previous message, tried 1.1.1.1
// think there was a race to handle this data from 4 threads
// must rework this if we want that warning back
// but, as comments below go, that warning is false, so replaced it to truer one
//	static struct previous_tag {
//		std::string name;
//		std::string key;
//	} previous;
	ssize_t written = -1;
	for(int attempt = 0; attempt < 2; attempt++) {
		written = send(mSocket, json_cstr, json_length, 0);
		if(written < 0 && (errno == ECONNREFUSED || errno == EINTR)) {
			// "probably" because it could be like this:
			// 1. send(key1). previous.key:=key1
			// 2. no ICMP error received yet
			// 3. remote server started OK
			// 4. send(key2). previous.key:=key2
			// 5. ICMP error arrives (relating to key1 received)
			// 6. send(key3) detects problem, but reports previous.key==key2 instead of key1
			//
			// due to this mess, maybe we should remove this confusing warning
			// leaving it here for now to see if we will ever see that
//			LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: probably this notification was not delivered. errno[%d]",
//				__func__, previous.name.c_str(), previous.key.c_str(), errno);

			// actually can not say from which bucket so will just give a general warning
			LOG(EXTENSION_LOG_WARNING, "%s[can not say, which bucket/key, sorry about that]: Previous notification was not delivered. errno[%d]",
				__func__, errno);
			continue;
		}
		break;
	}
	if(json_length != static_cast<size_t>(written)) {
		LOG(EXTENSION_LOG_WARNING, "%s[%s.%s]: json_length[%zu] != written[%zd] errno[%d]",
			__func__, name.c_str(), v->getKey().c_str(), json_length, written, errno);
	}

	cJSON_Free(json_cstr);

//	previous.name = name;
//	previous.key = v->getKey();
}

void ExpiryChannel::close() {
	if(isConnected()) {
		::close(mSocket);
		mSocket = -1;
	}
}

const bool ExpiryChannel::isConnected() const {
	return mSocket >= 0;
}
