// =================================================================================================
// eModbus: Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to eModbus
//               MIT license - see license.md for details
// =================================================================================================
#include "ModbusClientTCPasync.h"
#define LOCAL_LOG_LEVEL LOG_LEVEL_VERBOSE
// #undef LOCAL_LOG_LEVEL
#include "Logging.h"

ModbusClientTCPasync::ModbusClientTCPasync(IPAddress address, uint16_t port, uint16_t queueLimit) :
  ModbusClient(),
  txQueue(),
  rxQueue(),
  MTA_client(),
  MTA_timeout(DEFAULTTIMEOUT),
  MTA_idleTimeout(DEFAULTIDLETIME),
  MTA_qLimit(queueLimit),
  MTA_maxInflightRequests(queueLimit),
  MTA_lastActivity(0),
  MTA_state(DISCONNECTED),
  MTA_host(address),
  MTA_port(port)
    {
      // attach all handlers on async tcp events
      MTA_client.onConnect([](void* i, AsyncClient* c) { (static_cast<ModbusClientTCPasync*>(i))->onConnected(); }, this);
      MTA_client.onDisconnect([](void* i, AsyncClient* c) { (static_cast<ModbusClientTCPasync*>(i))->onDisconnected(); }, this);
      MTA_client.onError([](void* i, AsyncClient* c, int8_t error) { (static_cast<ModbusClientTCPasync*>(i))->onACError(c, error); }, this);
      // MTA_client.onTimeout([](void* i, AsyncClient* c, uint32_t time) { (static_cast<ModbusClientTCPasync*>(i))->onTimeout(time); }, this);
      // MTA_client.onAck([](void* i, AsyncClient* c, size_t len, uint32_t time) { (static_cast<ModbusClientTCPasync*>(i))->onAck(len, time); }, this);
      MTA_client.onData([](void* i, AsyncClient* c, void* data, size_t len) { (static_cast<ModbusClientTCPasync*>(i))->onPacket(static_cast<uint8_t*>(data), len); }, this);
      MTA_client.onPoll([](void* i, AsyncClient* c) { (static_cast<ModbusClientTCPasync*>(i))->onPoll(); }, this);

      // disable nagle algorithm ref Modbus spec
      MTA_client.setNoDelay(true);
    }

// Destructor: clean up queue, task etc.
ModbusClientTCPasync::~ModbusClientTCPasync() {
  // Clean up queue
  {
    // Safely lock access
    lock_guard<mutex> lockGuard(qLock);
    lock_guard<mutex> lockguard(sLock);
    // Delete all elements from queues
    while (!txQueue.empty()) {
      delete txQueue.front();
      txQueue.pop_front();
    }
    for (auto it = rxQueue.cbegin(); it != rxQueue.cend();/* no increment */) {
      delete it->second;
      it = rxQueue.erase(it);
    }
  }
  // force close client
  MTA_client.close(true);
}

// optionally manually connect to modbus server. Otherwise connection will be made upon first request
void ModbusClientTCPasync::connect() {
  LOG_D("connecting");
  lock_guard<mutex> lockGuard(sLock);
  // only connect if disconnected
  if (MTA_state == DISCONNECTED) {
    MTA_state = CONNECTING;
    MTA_client.connect(MTA_host, MTA_port);
  }
}

// manually disconnect from modbus server. Connection will also auto close after idle time
void ModbusClientTCPasync::disconnect(bool force) {
  LOG_D("disconnecting");
  MTA_client.close(force);
}

// Set timeout value
void ModbusClientTCPasync::setTimeout(uint32_t timeout) {
  MTA_timeout = timeout;
}

// Set idle timeout value (time before connection auto closes after being idle)
void ModbusClientTCPasync::setIdleTimeout(uint32_t timeout) {
  MTA_idleTimeout = timeout;
}

void ModbusClientTCPasync::setMaxInflightRequests(uint32_t maxInflightRequests) {
  MTA_maxInflightRequests = maxInflightRequests;
}

// Base addRequest for preformatted ModbusMessage and last set target
Error ModbusClientTCPasync::addRequest(ModbusMessage msg, uint32_t token) {
  Error rc = SUCCESS;        // Return value

  // Add it to the queue, if valid
  if (msg) {
    // Queue add successful?
    if (!addToQueue(token, msg)) {
      // No. Return error after deleting the allocated request.
      rc = REQUEST_QUEUE_FULL;
    }
  }

  LOG_D("Add TCP request result: %02X\n", rc);
  return rc;
}

// addToQueue: send freshly created request to queue
bool ModbusClientTCPasync::addToQueue(int32_t token, ModbusMessage request) {
  // Did we get one?
  if (request) {
    lock_guard<mutex> lockGuard(qLock);
    if (txQueue.size() + rxQueue.size() < MTA_qLimit) {
      HEXDUMP_V("Enqueue", request.data(), request.size());
      RequestEntry *re = new RequestEntry(token, request);
      re->head.transactionID = messageCount++;
      if (!re) return false;  //TODO: proper error returning in case allocation fails
      // inject proper transactionID
      re->head.transactionID = messageCount++;
      re->head.len = request.size();
      // if we're already connected, try to send and push to rxQueue
      // or else push to txQueue and (re)connect
      if (MTA_state == CONNECTED && send(re)) {
        rxQueue[re->head.transactionID] = re;
      } else {
        txQueue.push_back(re);
        if (MTA_state == DISCONNECTED) {
          connect();
        }
      }
      return true;
    }
    LOG_E("queue is full");
  }
  return false;
}

void ModbusClientTCPasync::onConnected() {
  LOG_D("connected");
  lock_guard<mutex> lockGuard(sLock);
  MTA_state = CONNECTED;
  MTA_lastActivity = millis();
  // from now on onPoll will be called every 500 msec
}

void ModbusClientTCPasync::onDisconnected() {
  LOG_D("disconnected");
  lock_guard<mutex> slockGuard(sLock);
  MTA_state = DISCONNECTED;

  // empty queue on disconnect, calling errorcode on every waiting request
  lock_guard<mutex> qlockGuard(qLock);
  while (!txQueue.empty()) {
    RequestEntry* r = txQueue.front();
    if (onError) {
      onError(IP_CONNECTION_FAILED, r->token);
    }
    delete r;
    txQueue.pop_front();
  }
  while (!rxQueue.empty()) {
    RequestEntry *r = rxQueue.begin()->second;
    if (onError) {
      onError(IP_CONNECTION_FAILED, r->token);
    }
    delete r;
    rxQueue.erase(rxQueue.begin());
  }
}


void ModbusClientTCPasync::onACError(AsyncClient* c, int8_t error) {
  // onDisconnect will alse be called, so nothing to do here
  LOG_W("TCP error: %s", c->errorToString(error));
}

/*
void onTimeout(uint32_t time) {
  // timeOut is handled by onPoll or onDisconnect
}

void onAck(size_t len, uint32_t time) {
  // assuming we don't need this
}
*/
void ModbusClientTCPasync::onPacket(uint8_t* data, size_t length) {
  LOG_D("packet received (len:%d)", length);
  // reset idle timeout
  MTA_lastActivity = millis();

  while (length > 0) {
    LOG_D("parsing (len:%d)", length + 1);

    RequestEntry* request = nullptr;
    ModbusMessage* response = nullptr;
    uint16_t transactionID = 0;
    uint16_t messageLength = 0;

    // 1. Check for valid modbus message

    // MBAP header is 6 bytes, we can't do anything with less
    // total message should be longer then byte 5 (remaining length) + MBAP length
    // remaining length should be less then 254
    if (length > 6 &&
        data[2] == 0 &&
        data[3] == 0 &&
        length >= data[5] + 6 &&
        data[5] < 254) {
      transactionID = data[0] << 8 | data[1];
      messageLength = data[4] << 8 | data[5];
      response = new ModbusMessage(messageLength);
      response->add(&data[6], messageLength);
      LOG_D("packet validated (len:%d)", messageLength);

      // on next iteration: adjust remaining lengt and pointer to data
      length -= 6 + messageLength;
      data += 6 + messageLength;
    } else {
      // invalid packet, abort function
      LOG_W("packet invalid");
      // try again skipping the first byte
      --length;
      return;
    }

    // 2. if we got a valid response, match with a request

    if (response) {
      lock_guard<mutex> lockGuard(qLock);
      auto i = rxQueue.find(transactionID);
      if (i != rxQueue.end()) {
        // found it, handle it and stop iterating
        request = i->second;
        i = rxQueue.erase(i);
        LOG_D("matched request");
      } else {
        // TCP packet did not yield valid modbus response, abort function
        LOG_W("no matching request found");
        return;
      }
    } else {
      // response was not set
      return;
    }

    // 3. we have a valid request and a valid response, call appropriate callback

    if (request) {
      // compare request with response
      Error error = SUCCESS;
      if (request->msg.getFunctionCode() != response->getFunctionCode()) {
        error = FC_MISMATCH;
      } else if (request->msg.getServerID() != response->getServerID()) {
        error = SERVER_ID_MISMATCH;
      } else {
        error = response->getError();
      }
      if (error == SUCCESS) {
        if (onData) {
          onData(*response, request->token);
        }
      } else {
        if (onError) {
          onError(response->getError(), request->token);
        }
      }
      delete request;
    }
    delete response;

  }  // end processing of incoming data

  // check if we have to send the next request
  lock_guard<mutex> lockGuard(qLock);
  handleSendingQueue();
}

void ModbusClientTCPasync::onPoll() {
  {
  lock_guard<mutex> lockGuard(qLock);

  LOG_D("Queue sizes: tx:%d rx:%d", txQueue.size(), rxQueue.size());

  // try to send whatever is waiting
  handleSendingQueue();

  // next check if timeout has struck for oldest request
  if (!rxQueue.empty()) {
    RequestEntry* request = rxQueue.begin()->second;
    if (millis() - request->timeout > MTA_timeout) {
      LOG_D("request timeouts");
      // oldest element timeouts, call onError and clean up
      if (onError) {
        // Handle timeout error
        onError(TIMEOUT, request->token);
      }
      delete request;
      rxQueue.erase(rxQueue.begin());
    }
  }
    
  }  // end lockguard scope

  // if nothing happened during idle timeout, gracefully close connection
  if (millis() - MTA_lastActivity > MTA_idleTimeout) {
    disconnect();
  }
}

void ModbusClientTCPasync::handleSendingQueue() {
  // ATTENTION: This method does not have a lock guard.
  // Calling sites must assure shared resources are protected
  // by mutex.

  // try to send everything we have waiting
  std::list<RequestEntry*>::iterator i = txQueue.begin();
  while (i != txQueue.end()) {
    // get the actual element
    RequestEntry* r = *i;
    if (send(r)) {
      // after sending, update timeout value, add to other queue and remove from this queue
      r->timeout = millis();
      rxQueue[r->head.transactionID] = r;      // push request to other queue
      i = txQueue.erase(i);  // remove from toSend queue and point i to next request
    } else {
      // sending didn't succeed, try next request
      ++i;
    }
  }
}

bool ModbusClientTCPasync::send(RequestEntry* re) {
  // ATTENTION: This method does not have a lock guard.
  // Calling sites must assure shared resources are protected
  // by mutex.

  if (rxQueue.size() >= MTA_maxInflightRequests)
    return false;

  // check if TCP client is able to send
  if (MTA_client.space() > re->msg.size() + 6) {
    // Write TCP header first
    MTA_client.add(reinterpret_cast<const char *>((const uint8_t *)(re->head)), 6);
    // Request comes next
    MTA_client.add(reinterpret_cast<const char*>(re->msg.data()), re->msg.size());
    // done
    MTA_client.send();
    // reset idle timeout
    LOG_D("request sent (msgid:%d)", re->head.transactionID);
    return true;
  }
  return false;
}
