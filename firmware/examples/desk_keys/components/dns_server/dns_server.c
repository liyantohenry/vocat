/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <sys/param.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "dns_server.h"

#define DNS_PORT (53)
#define DNS_MAX_LEN (512)

#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (10)      /* short: the answers are only right on the setup network */

static const char *TAG = "example_dns_redirect_server";

// DNS Header Packet
typedef struct __attribute__((__packed__))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

// DNS Question Packet
typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

// DNS Answer Packet
typedef struct __attribute__((__packed__))
{
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

// DNS server handle
struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

/*
 * Builds the reply to a DNS request. Changed from the ESP-IDF example, which answered every question as an
 * A record and appended the answer after the whole request: AAAA/HTTPS queries got a reply claiming an
 * answer it didn't contain, and queries with an EDNS OPT record got the answer after the OPT record, where
 * resolvers don't look. Phones read those broken replies as a broken network and left the setup network.
 *
 * Now: copy the header and the one question, answer an A (or ANY) question with the rule's IPv4 address,
 * answer anything else with no records (NODATA), and drop the additional records.
 */
static int parse_dns_request(char *req, size_t req_len, char *dns_reply, size_t dns_reply_max_len, dns_server_handle_t h)
{
    const uint8_t *q = (const uint8_t *)req;
    if (req_len < sizeof(dns_header_t)) {
        return -1;
    }
    const dns_header_t *req_header = (const dns_header_t *)req;
    uint16_t flags = ntohs(req_header->flags);
    if ((flags & 0x8000) || (flags & 0x7800) || ntohs(req_header->qd_count) != 1) {
        return 0;   /* a response, not a standard query, or not exactly one question: ignore */
    }

    /* Walk the question name (uncompressed labels) */
    size_t pos = sizeof(dns_header_t);
    char name[128];
    size_t name_len = 0;
    while (pos < req_len && q[pos] != 0) {
        uint8_t label = q[pos];
        if ((label & 0xC0) || pos + 1 + label > req_len || name_len + label + 1 >= sizeof(name)) {
            return -1;
        }
        if (name_len) {
            name[name_len++] = '.';
        }
        memcpy(name + name_len, q + pos + 1, label);
        name_len += label;
        pos += 1 + label;
    }
    name[name_len] = '\0';
    pos++;      /* the name's terminating zero */
    if (pos + sizeof(dns_question_t) > req_len) {
        return -1;
    }
    uint16_t qd_type = (q[pos] << 8) | q[pos + 1];
    size_t question_end = pos + sizeof(dns_question_t);
    if (question_end + sizeof(dns_answer_t) > dns_reply_max_len) {
        return -1;
    }

    /* Header and question, without the request's additional (EDNS) records */
    memcpy(dns_reply, req, question_end);
    dns_header_t *header = (dns_header_t *)dns_reply;
    header->flags = htons(0x8000 | 0x0400 | (flags & 0x0100) | 0x0080);    /* QR, AA, RD copied, RA; NOERROR */
    header->an_count = 0;
    header->ns_count = 0;
    header->ar_count = 0;

    esp_ip4_addr_t ip = { .addr = IPADDR_ANY };
    if (qd_type == QD_TYPE_A || qd_type == 255 /* ANY */) {
        for (int i = 0; i < h->num_of_entries; ++i) {
            if (strcmp(h->entry[i].name, "*") == 0 || strcmp(h->entry[i].name, name) == 0) {
                if (h->entry[i].if_key) {
                    esp_netif_ip_info_t ip_info;
                    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey(h->entry[i].if_key), &ip_info);
                    ip.addr = ip_info.ip.addr;
                } else {
                    ip.addr = h->entry[i].ip.addr;
                }
                break;
            }
        }
    }
    ESP_LOGD(TAG, "Question type %u for %s -> %s", qd_type, name, ip.addr != IPADDR_ANY ? "answered" : "no records");
    if (ip.addr == IPADDR_ANY) {
        return question_end;
    }

    dns_answer_t *answer = (dns_answer_t *)(dns_reply + question_end);
    answer->ptr_offset = htons(0xC000 | sizeof(dns_header_t));
    answer->type = htons(QD_TYPE_A);
    answer->class = htons(1);
    answer->ttl = htonl(ANS_TTL_SEC);
    answer->addr_len = htons(sizeof(ip.addr));
    answer->ip_addr = ip.addr;
    header->an_count = htons(1);
    return question_end + sizeof(dns_answer_t);
}

/*
    Sets up a socket and listen for DNS queries,
    replies to all type A queries with the IP of the softAP
*/
void dns_server_task(void *pvParameters)
{
    char rx_buffer[DNS_MAX_LEN];
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    dns_server_handle_t handle = pvParameters;

    while (handle->started) {

        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(DNS_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket bound, port %d", DNS_PORT);

        while (handle->started) {
            struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                close(sock);
                break;
            }
            // Data received
            else {
                // Get the sender's ip address as string
                if (source_addr.sin6_family == PF_INET) {
                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                } else if (source_addr.sin6_family == PF_INET6) {
                    inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
                }

                // Null-terminate whatever we received and treat like a string...
                rx_buffer[len] = 0;

                char reply[DNS_MAX_LEN];
                int reply_len = parse_dns_request(rx_buffer, len, reply, DNS_MAX_LEN, handle);

                ESP_LOGD(TAG, "Received %d bytes from %s | DNS reply with len: %d", len, addr_str, reply_len);
                if (reply_len < 0) {
                    ESP_LOGW(TAG, "Couldn't parse a %d-byte DNS request from %s", len, addr_str);
                } else if (reply_len > 0) {
                    int err = sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
                    if (err < 0) {
                        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                        break;
                    }
                }
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    dns_server_handle_t handle = calloc(1, sizeof(struct dns_server_handle) + config->num_of_entries * sizeof(dns_entry_pair_t));
    ESP_RETURN_ON_FALSE(handle, NULL, TAG, "Failed to allocate dns server handle");

    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item, config->num_of_entries * sizeof(dns_entry_pair_t));

    xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task);
    return handle;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if (handle) {
        handle->started = false;
        vTaskDelete(handle->task);
        free(handle);
    }
}
