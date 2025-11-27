#ifndef MQTT_HELPERS_H
#define MQTT_HELPERS_H

#include <PubSubClient.h>

void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnect(PubSubClient& client, const char* clientId, const char* mqtt_user, const char* mqtt_password);
const char* mqttStateToString(int8_t state);

#endif // MQTT_HELPERS_H