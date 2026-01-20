#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h> // Using Jansson for JSON handling
#include <stdint.h> // For uint32_t
#include <pthread.h>

double current_virtual_time = 0.0; // Definition of current_virtual_time

// Global counter for event IDs
static unsigned long next_event_id = 0;
pthread_mutex_t event_id_mutex = PTHREAD_MUTEX_INITIALIZER;

unsigned long generate_event_id() {
    pthread_mutex_lock(&event_id_mutex);
    unsigned long id = next_event_id++;
    pthread_mutex_unlock(&event_id_mutex);
    return id;
}

// --- JSON Serialization/Deserialization (using Jansson library) ---

char* event_to_json(const Event* event) {
    json_t *root = json_object();
    json_object_set_new(root, "timestamp", json_real(event->timestamp));
    json_object_set_new(root, "router_id", json_integer(event->router_id));
    json_object_set_new(root, "thread_id", json_integer(event->thread_id));
    json_object_set_new(root, "event_type", json_string(
        event->event_type == ROUTER_START ? "ROUTER_START" :
        event->event_type == ROUTER_BLOCK_REQUEST ? "ROUTER_BLOCK_REQUEST" :
        event->event_type == PACKET_SEND_EVENT ? "PACKET_SEND_EVENT" :
        event->event_type == PACKET_RECEIVE_EVENT ? "PACKET_RECEIVE_EVENT" :
        event->event_type == TIMEOUT_EVENT ? "TIMEOUT_EVENT" :
        event->event_type == CONNECT_REQUEST_EVENT ? "CONNECT_REQUEST_EVENT" :
        event->event_type == CONNECTION_ESTABLISHED_EVENT ? "CONNECTION_ESTABLISHED_EVENT" :
        event->event_type == LISTEN_EVENT ? "LISTEN_EVENT" :
        event->event_type == CONNECTION_INFO_EVENT ? "CONNECTION_INFO_EVENT" :
        event->event_type == CLOSE_SOCKET_EVENT ? "CLOSE_SOCKET_EVENT" : "UNKNOWN"
    ));
    json_object_set_new(root, "event_id", json_integer(event->event_id));
    
    json_error_t error;
    json_t *payload_obj = json_loads(event->payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "Error parsing payload JSON in event_to_json: %s\n", error.text);
        json_object_set_new(root, "payload", json_string("{}")); // Fallback to empty JSON
    } else {
        json_object_set_new(root, "payload", payload_obj); // Pass the parsed object
    }

    char *json_dump = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return json_dump;
}

void json_to_event(const char* json_str, Event* event) {
    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing event JSON: %s (line %d, col %d)\n", error.text, error.line, error.column);
        return;
    }

    event->timestamp = json_real_value(json_object_get(root, "timestamp"));
    event->router_id = json_integer_value(json_object_get(root, "router_id"));
    
    json_t *event_type_json = json_object_get(root, "event_type");
    const char* event_type_str = event_type_json ? json_string_value(event_type_json) : NULL;
    if (event_type_str && strcmp(event_type_str, "ROUTER_START") == 0) event->event_type = ROUTER_START;
    else if (event_type_str && strcmp(event_type_str, "ROUTER_BLOCK_REQUEST") == 0) event->event_type = ROUTER_BLOCK_REQUEST;
    else if (event_type_str && strcmp(event_type_str, "PACKET_SEND_EVENT") == 0) event->event_type = PACKET_SEND_EVENT;
    else if (event_type_str && strcmp(event_type_str, "PACKET_RECEIVE_EVENT") == 0) event->event_type = PACKET_RECEIVE_EVENT;
    else if (event_type_str && strcmp(event_type_str, "TIMEOUT_EVENT") == 0) event->event_type = TIMEOUT_EVENT;
    else if (event_type_str && strcmp(event_type_str, "CONNECT_REQUEST_EVENT") == 0) event->event_type = CONNECT_REQUEST_EVENT;
    else if (event_type_str && strcmp(event_type_str, "CONNECTION_ESTABLISHED_EVENT") == 0) event->event_type = CONNECTION_ESTABLISHED_EVENT;
    else if (event_type_str && strcmp(event_type_str, "LISTEN_EVENT") == 0) event->event_type = LISTEN_EVENT;
    else if (event_type_str && strcmp(event_type_str, "CONNECTION_INFO_EVENT") == 0) event->event_type = CONNECTION_INFO_EVENT;
    else if (event_type_str && strcmp(event_type_str, "CLOSE_SOCKET_EVENT") == 0) event->event_type = CLOSE_SOCKET_EVENT;
    else if (event_type_str && strcmp(event_type_str, "GET_VIRTUAL_TIME_EVENT") == 0) event->event_type = GET_VIRTUAL_TIME_EVENT;
    else event->event_type = -1; // Unknown

    event->event_id = json_integer_value(json_object_get(root, "event_id"));
    event->thread_id = json_integer_value(json_object_get(root, "thread_id"));
    
    json_t *payload_obj = json_object_get(root, "payload");
    if (payload_obj) {
        char *payload_dump = json_dumps(payload_obj, JSON_COMPACT);
        if (payload_dump) {
            strncpy(event->payload.json_str, payload_dump, MAX_MSG_SIZE - 1);
            event->payload.json_str[MAX_MSG_SIZE - 1] = '\0';
            free(payload_dump);
        } else {
            strcpy(event->payload.json_str, "{}");
        }
    } else {
        strcpy(event->payload.json_str, "{}");
    }

    json_decref(root);
}

char* message_to_json(const Message* msg) {
    json_t *root = json_object();
    json_object_set_new(root, "message_type", json_string(
        msg->message_type == HOOK_TO_DESD ? "HOOK_TO_DESD" : "DESD_TO_HOOK"
    ));
    json_object_set_new(root, "router_id", json_integer(msg->router_id));
    json_object_set_new(root, "thread_id", json_integer(msg->thread_id));
    json_object_set_new(root, "request_id", json_string(msg->request_id));
    json_object_set_new(root, "virtual_time", json_real(msg->virtual_time));
    
    json_error_t error;
    json_t *payload_obj = json_loads(msg->payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "Error parsing payload JSON in message_to_json: %s\n", error.text);
        json_object_set_new(root, "payload", json_string("{}")); // Fallback to empty JSON
    } else {
        json_object_set_new(root, "payload", payload_obj);
    }

    if (msg->message_type == HOOK_TO_DESD) {
        json_object_set_new(root, "event_type", json_string(
            msg->event_type == ROUTER_START ? "ROUTER_START" :
            msg->event_type == ROUTER_BLOCK_REQUEST ? "ROUTER_BLOCK_REQUEST" :
            msg->event_type == PACKET_SEND_EVENT ? "PACKET_SEND_EVENT" :
            msg->event_type == PACKET_RECEIVE_EVENT ? "PACKET_RECEIVE_EVENT" :
            msg->event_type == TIMEOUT_EVENT ? "TIMEOUT_EVENT" :
            msg->event_type == CONNECT_REQUEST_EVENT ? "CONNECT_REQUEST_EVENT" :
            msg->event_type == CONNECTION_ESTABLISHED_EVENT ? "CONNECTION_ESTABLISHED_EVENT" :
            msg->event_type == LISTEN_EVENT ? "LISTEN_EVENT" :
            msg->event_type == CONNECTION_INFO_EVENT ? "CONNECTION_INFO_EVENT" :
            msg->event_type == CLOSE_SOCKET_EVENT ? "CLOSE_SOCKET_EVENT" :
            msg->event_type == CANCEL_BLOCK_REQUEST ? "CANCEL_BLOCK_REQUEST" :
            msg->event_type == GET_VIRTUAL_TIME_EVENT ? "GET_VIRTUAL_TIME_EVENT" : "UNKNOWN"
        ));
    }

    char *json_dump = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return json_dump;
}

void json_to_message(const char* json_str, Message* msg) {
    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing message JSON: %s (line %d, col %d)\n", error.text, error.line, error.column);
        return;
    }

    json_t *message_type_json = json_object_get(root, "message_type");
    const char* message_type_str = message_type_json ? json_string_value(message_type_json) : NULL;
    if (message_type_str && strcmp(message_type_str, "HOOK_TO_DESD") == 0) msg->message_type = HOOK_TO_DESD;
    else if (message_type_str && strcmp(message_type_str, "DESD_TO_HOOK") == 0) msg->message_type = DESD_TO_HOOK;
    else msg->message_type = -1; // Unknown

    msg->router_id = json_integer_value(json_object_get(root, "router_id"));
    msg->thread_id = json_integer_value(json_object_get(root, "thread_id"));
    
    json_t *request_id_json = json_object_get(root, "request_id");
    const char *request_id_str = request_id_json ? json_string_value(request_id_json) : NULL;
    if (request_id_str) {
        strncpy(msg->request_id, request_id_str, 63);
        msg->request_id[63] = '\0';
    } else {
        msg->request_id[0] = '\0';
    }
    
    msg->virtual_time = json_real_value(json_object_get(root, "virtual_time"));

    if (msg->message_type == HOOK_TO_DESD) {
        json_t *event_type_json = json_object_get(root, "event_type");
        const char* event_type_str = event_type_json ? json_string_value(event_type_json) : NULL;
        if (event_type_str && strcmp(event_type_str, "ROUTER_START") == 0) msg->event_type = ROUTER_START;
        else if (event_type_str && strcmp(event_type_str, "ROUTER_BLOCK_REQUEST") == 0) msg->event_type = ROUTER_BLOCK_REQUEST;
        else if (event_type_str && strcmp(event_type_str, "PACKET_SEND_EVENT") == 0) msg->event_type = PACKET_SEND_EVENT;
        else if (event_type_str && strcmp(event_type_str, "PACKET_RECEIVE_EVENT") == 0) msg->event_type = PACKET_RECEIVE_EVENT;
        else if (event_type_str && strcmp(event_type_str, "TIMEOUT_EVENT") == 0) msg->event_type = TIMEOUT_EVENT;
        else if (event_type_str && strcmp(event_type_str, "CONNECT_REQUEST_EVENT") == 0) msg->event_type = CONNECT_REQUEST_EVENT;
        else if (event_type_str && strcmp(event_type_str, "CONNECTION_ESTABLISHED_EVENT") == 0) msg->event_type = CONNECTION_ESTABLISHED_EVENT;
        else if (event_type_str && strcmp(event_type_str, "LISTEN_EVENT") == 0) msg->event_type = LISTEN_EVENT;
        else if (event_type_str && strcmp(event_type_str, "CONNECTION_INFO_EVENT") == 0) msg->event_type = CONNECTION_INFO_EVENT;
        else if (event_type_str && strcmp(event_type_str, "CLOSE_SOCKET_EVENT") == 0) msg->event_type = CLOSE_SOCKET_EVENT;
        else if (event_type_str && strcmp(event_type_str, "CANCEL_BLOCK_REQUEST") == 0) msg->event_type = CANCEL_BLOCK_REQUEST;
        else if (event_type_str && strcmp(event_type_str, "GET_VIRTUAL_TIME_EVENT") == 0) msg->event_type = GET_VIRTUAL_TIME_EVENT;
        else msg->event_type = -1; // Unknown
    }

    json_t *payload_obj = json_object_get(root, "payload");
    if (payload_obj) {
        char *payload_dump = json_dumps(payload_obj, JSON_COMPACT);
        if (payload_dump) {
            strncpy(msg->payload.json_str, payload_dump, MAX_MSG_SIZE - 1);
            msg->payload.json_str[MAX_MSG_SIZE - 1] = '\0';
            free(payload_dump);
        } else {
            strcpy(msg->payload.json_str, "{}");
        }
    } else {
        strcpy(msg->payload.json_str, "{}");
    }

    json_decref(root);
}

// --- Base64 Encoding/Decoding (simplified for brevity, a full implementation would be more complex) ---

static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="; // Added '=' for padding

char* base64_encode(const unsigned char* data, size_t input_length) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = (char*) malloc(output_length + 1);
    if (encoded_data == NULL) return NULL;

    int i = 0, j = 0;
    for (i = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }

    // Add padding
    for (int k = 0; k < (3 - input_length % 3) % 3; ++k) {
        encoded_data[output_length - 1 - k] = '=';
    }
    encoded_data[output_length] = '\0';
    return encoded_data;
}

// A lookup table for decoding
static const int decoding_table[] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0-15
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 16-31
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, // 32-47
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1, // 48-63 (0-9)
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 64-79 (A-O)
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, // 80-95 (P-Z)
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 96-111 (a-o)
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // 112-127 (p-z)
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 128-143
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 144-159
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 160-175
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 176-191
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 192-207
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 208-223
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 224-239
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1  // 240-255
};

unsigned char* base64_decode(const char* data, size_t *output_length) {
    size_t input_length = strlen(data);
    if (input_length == 0) {
        *output_length = 0;
        return (unsigned char*)strdup("");
    }

    // Calculate decoded length, accounting for padding
    size_t padding = 0;
    if (input_length > 0 && data[input_length - 1] == '=') padding++;
    if (input_length > 1 && data[input_length - 2] == '=') padding++;

    *output_length = (input_length / 4 * 3) - padding;
    unsigned char *decoded_data = (unsigned char*) malloc(*output_length + 1);
    if (decoded_data == NULL) return NULL;

    int i = 0, j = 0;
    for (i = 0; i < input_length;) {
        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];

        uint32_t triple = (sextet_a << 3 * 6)
                        + (sextet_b << 2 * 6)
                        + (sextet_c << 1 * 6)
                        + (sextet_d << 0 * 6);

        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    decoded_data[*output_length] = '\0';
    return decoded_data;
}
