#ifndef CAN_EMULATOR

#include "usbutil.h"
#include "canread.h"
#include "serialutil.h"
#include "signals.h"
#include "log.h"
#include "cJSON.h"
#include "listener.h"
#include <stdint.h>

extern SerialDevice SERIAL_DEVICE;
extern UsbDevice USB_DEVICE;
extern Listener listener;

/* Forward declarations */

void receiveCan(CanBus*);
void initializeAllCan();
bool receiveWriteRequest(uint8_t*);

void setup() {
    initializeLogging();
#ifndef NO_UART
    initializeSerial(&SERIAL_DEVICE);
#endif
    initializeUsb(&USB_DEVICE);
    initializeAllCan();
}

void loop() {
    for(int i = 0; i < getCanBusCount(); i++) {
        receiveCan(&getCanBuses()[i]);
    }
    processListenerQueues(&listener);
    readFromHost(&USB_DEVICE, &receiveWriteRequest);
#ifndef NO_UART
    readFromSerial(&SERIAL_DEVICE, &receiveWriteRequest);
#endif
    for(int i = 0; i < getCanBusCount(); i++) {
        processCanWriteQueue(&getCanBuses()[i]);
    }
}

void initializeAllCan() {
    for(int i = 0; i < getCanBusCount(); i++) {
        initializeCan(&(getCanBuses()[i]));
    }
}

void receiveRawWriteRequest(cJSON* idObject, cJSON* root) {
    uint32_t id = idObject->valueint;
    cJSON* dataObject = cJSON_GetObjectItem(root, "data");
    if(dataObject == NULL) {
        debug("Raw write request missing data\r\n", id);
        return;
    }

    char* dataString = dataObject->valuestring;
    char* end;
    unsigned long long data = __builtin_bswap64(strtoull(dataString, &end, 16));
    CanMessage message = {id, data};
    QUEUE_PUSH(CanMessage, &getCanBuses()[0].sendQueue, message);
}

/* The binary format handled by this function is as follows:
 *
 * A leading '{' followed by a 4 byte message ID, then a '|' separator and
 * finally 8 bytes of data and a trailing '}'. E.g.:
 *
 * {<4 byte ID>|<8 bytes of data>}
 */
void receiveBinaryWriteRequest(uint8_t* message) {
    int index = 0;
    const int BINARY_CAN_WRITE_PACKET_LENGTH = 15;
    debug(".");
    while((message[index] != '!')  && (index + BINARY_CAN_WRITE_PACKET_LENGTH < 64)) {

        if(message[index] != '{' || message[index+5] != '|'
                || message[index+14] != '}') {
            debug("Received a corrupted CAN message.\r\n");
            for(int i = 0; i < 16; i++) {
                debug("%02x ", message[index+i] );
            }
            debug("\r\n");
            continue;
        }

        CanMessage outgoing = {0, 0};
        memcpy((uint8_t*)&outgoing.id, &message[index+1], 4);
        for(int i = 0; i < 8; i++) {
            ((uint8_t*)&(outgoing.data))[i] = message[index+i+6];
        }
        QUEUE_PUSH(CanMessage, &getCanBuses()[0].sendQueue, outgoing);
        index += BINARY_CAN_WRITE_PACKET_LENGTH;
    }
}

void receiveTranslatedWriteRequest(cJSON* nameObject, cJSON* root) {
    char* name = nameObject->valuestring;
    cJSON* value = cJSON_GetObjectItem(root, "value");
    if(value == NULL) {
        debug("Write request for %s missing value\r\n", name);
        return;
    }

    CanSignal* signal = lookupSignal(name, getSignals(),
            getSignalCount(), true);
    CanCommand* command = lookupCommand(name, getCommands(),
            getCommandCount());
    if(signal != NULL) {
        sendCanSignal(signal, value, getSignals(), getSignalCount());
    } else if(command != NULL) {
        command->handler(name, value, getSignals(), getSignalCount());
    } else {
        debug("Writing not allowed for signal with name %s\r\n", name);
    }
}

bool receiveJsonWriteRequest(uint8_t* message) {
    cJSON *root = cJSON_Parse((char*)message);
    bool foundMessage = false;
    if(root != NULL) {
        foundMessage = true;
        cJSON* nameObject = cJSON_GetObjectItem(root, "name");
        if(nameObject == NULL) {
            cJSON* idObject = cJSON_GetObjectItem(root, "id");
            if(idObject == NULL) {
                debug("Write request is malformed, "
                        "missing name or id: %s\r\n", message);
            } else {
                receiveRawWriteRequest(idObject, root);
            }
        } else {
            receiveTranslatedWriteRequest(nameObject, root);
        }
        cJSON_Delete(root);
    } else {
        debug("Unable to parse JSON from \"%s\" -- if it's valid, "
                "may be out of memory\r\n", message);
    }
    return foundMessage;
}

bool receiveWriteRequest(uint8_t* message) {
#ifdef TRANSMITTER
    receiveBinaryWriteRequest(message);
    return true;
#else
    return receiveJsonWriteRequest(message);
#endif
}

/*
 * Check to see if a packet has been received. If so, read the packet and print
 * the packet payload to the serial monitor.
 */
void receiveCan(CanBus* bus) {
    // TODO what happens if we process until the queue is empty?
    if(!QUEUE_EMPTY(CanMessage, &bus->receiveQueue)) {
        CanMessage message = QUEUE_POP(CanMessage, &bus->receiveQueue);
        decodeCanMessage(message.id, message.data);
    }
}

void reset() {
    initializeAllCan();
}

#endif // CAN_EMULATOR
