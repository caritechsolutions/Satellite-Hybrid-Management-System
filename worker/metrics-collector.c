/*
 * RIST Metrics Collector - High Performance C Implementation
 * Collects metrics from RIST transports and stores in Redis TimeSeries
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <curl/curl.h>
#include <hiredis/hiredis.h>
#include <signal.h>

#define MAX_TRANSPORTS 100
#define MAX_PEERS 50
#define BUFFER_SIZE 1048576
#define CONFIG_PATH "/var/www/html/rist-monitor/config/transports.json"

typedef struct {
    char transport_id[64];
    int metrics_port;
    int active;
    pthread_t thread;
} Transport;

typedef struct {
    char peer_id[32];
    char cname[128];
    char listening[64];
    double bandwidth_mbps;
    double quality;
    double rtt_ms;
    long sent_packets;
    long retransmitted_packets;
    long received_packets;
    double retry_bandwidth_mbps;
} Peer;

typedef struct {
    char *data;
    size_t size;
} MemoryStruct;

// Global variables
static redisContext *redis_ctx = NULL;
static int running = 1;
static Transport transports[MAX_TRANSPORTS];
static int transport_count = 0;
static pthread_mutex_t redis_mutex = PTHREAD_MUTEX_INITIALIZER;

// Signal handler
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

// HTTP response callback
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if(ptr == NULL) {
        fprintf(stderr, "Not enough memory\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

// Fetch metrics from Prometheus endpoint
char* fetch_metrics(int port) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk = {0};
    chunk.data = malloc(1);
    chunk.size = 0;

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/metrics", port);

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if(res != CURLE_OK) {
            free(chunk.data);
            return NULL;
        }
    }

    return chunk.data;
}

// Parse Prometheus metrics format
int parse_metrics(const char *data, Peer *peers, int max_peers) {
    if(!data) return 0;

    int peer_count = 0;
    char *line_data = strdup(data);
    char *saveptr;
    char *token = strtok_r(line_data, "\n", &saveptr);

    while(token != NULL && peer_count < max_peers) {
        // Skip comments and empty lines
        if(token[0] == '#' || token[0] == '\0') {
            token = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        // Parse metric line: metric_name{labels} value
        char metric_name[256];
        char labels[512];
        double value;

        if(sscanf(token, "%255[^{]{%511[^}]} %lf", metric_name, labels, &value) == 3) {
            // Extract peer_id from labels
            char peer_id[32] = {0};
            char *peer_id_start = strstr(labels, "peer_id=\"");
            if(peer_id_start) {
                sscanf(peer_id_start + 9, "%31[^\"]", peer_id);

                // Find or create peer
                int peer_idx = -1;
                for(int i = 0; i < peer_count; i++) {
                    if(strcmp(peers[i].peer_id, peer_id) == 0) {
                        peer_idx = i;
                        break;
                    }
                }

                if(peer_idx == -1) {
                    peer_idx = peer_count++;
                    memset(&peers[peer_idx], 0, sizeof(Peer));
                    strncpy(peers[peer_idx].peer_id, peer_id, sizeof(peers[peer_idx].peer_id) - 1);

                    // Extract other labels
                    char *cname_start = strstr(labels, "cname=\"");
                    if(cname_start) {
                        sscanf(cname_start + 7, "%127[^\"]", peers[peer_idx].cname);
                    }

                    char *listening_start = strstr(labels, "listening=\"");
                    if(listening_start) {
                        sscanf(listening_start + 11, "%63[^\"]", peers[peer_idx].listening);
                    }
                }

                // Store metric value
                if(strstr(metric_name, "bandwidth_bps")) {
                    peers[peer_idx].bandwidth_mbps = value / 1000000.0;
                }
                else if(strstr(metric_name, "retry_bandwidth_bps")) {
                    peers[peer_idx].retry_bandwidth_mbps = value / 1000000.0;
                }
                else if(strstr(metric_name, "quality")) {
                    peers[peer_idx].quality = value;
                }
                else if(strstr(metric_name, "rtt_seconds")) {
                    peers[peer_idx].rtt_ms = value * 1000.0;
                }
                else if(strstr(metric_name, "sent_packets")) {
                    peers[peer_idx].sent_packets = (long)value;
                }
                else if(strstr(metric_name, "retransmitted_packets")) {
                    peers[peer_idx].retransmitted_packets = (long)value;
                }
                else if(strstr(metric_name, "received_packets")) {
                    peers[peer_idx].received_packets = (long)value;
                }
            }
        }

        token = strtok_r(NULL, "\n", &saveptr);
    }

    free(line_data);
    return peer_count;
}

// Store metrics in Redis TimeSeries
void store_metrics(const char *transport_id, Peer *peers, int peer_count) {
    if(!redis_ctx) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long timestamp = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;

    pthread_mutex_lock(&redis_mutex);

    for(int i = 0; i < peer_count; i++) {
        char key[256];
        redisReply *reply;

        // Store bandwidth
        snprintf(key, sizeof(key), "metrics:%s:%s:bandwidth",
                 transport_id, peers[i].peer_id);
        reply = redisCommand(redis_ctx, "TS.ADD %s %lld %.3f RETENTION 86400000",
                            key, timestamp, peers[i].bandwidth_mbps);
        if(reply) freeReplyObject(reply);

        // Store quality
        snprintf(key, sizeof(key), "metrics:%s:%s:quality",
                 transport_id, peers[i].peer_id);
        reply = redisCommand(redis_ctx, "TS.ADD %s %lld %.2f RETENTION 86400000",
                            key, timestamp, peers[i].quality);
        if(reply) freeReplyObject(reply);

        // Store RTT
        snprintf(key, sizeof(key), "metrics:%s:%s:rtt",
                 transport_id, peers[i].peer_id);
        reply = redisCommand(redis_ctx, "TS.ADD %s %lld %.2f RETENTION 86400000",
                            key, timestamp, peers[i].rtt_ms);
        if(reply) freeReplyObject(reply);

        // Store packet loss percentage
        double packet_loss = 0.0;
        if(peers[i].sent_packets > 0) {
            packet_loss = (peers[i].retransmitted_packets * 100.0) / peers[i].sent_packets;
        }
        snprintf(key, sizeof(key), "metrics:%s:%s:packet_loss",
                 transport_id, peers[i].peer_id);
        reply = redisCommand(redis_ctx, "TS.ADD %s %lld %.2f RETENTION 86400000",
                            key, timestamp, packet_loss);
        if(reply) freeReplyObject(reply);

        // Store retry bandwidth
        snprintf(key, sizeof(key), "metrics:%s:%s:retry_bandwidth",
                 transport_id, peers[i].peer_id);
        reply = redisCommand(redis_ctx, "TS.ADD %s %lld %.3f RETENTION 86400000",
                            key, timestamp, peers[i].retry_bandwidth_mbps);
        if(reply) freeReplyObject(reply);
    }

    pthread_mutex_unlock(&redis_mutex);
}

// Worker thread for each transport
void* transport_worker(void *arg) {
    Transport *transport = (Transport *)arg;

    printf("[%s] Worker started (port %d)\n",
           transport->transport_id, transport->metrics_port);

    while(running && transport->active) {
        // Fetch metrics
        char *metrics_data = fetch_metrics(transport->metrics_port);

        if(metrics_data) {
            // Parse metrics
            Peer peers[MAX_PEERS];
            memset(peers, 0, sizeof(peers));
            int peer_count = parse_metrics(metrics_data, peers, MAX_PEERS);

            // Store in Redis
            if(peer_count > 0) {
                store_metrics(transport->transport_id, peers, peer_count);
                printf("[%s] Collected %d peers (%.2f Mbps total)\n",
                       transport->transport_id, peer_count,
                       peers[0].bandwidth_mbps); // Show first peer's bandwidth
            } else {
                printf("[%s] No peers found\n", transport->transport_id);
            }

            free(metrics_data);
        } else {
            printf("[%s] Failed to fetch metrics from port %d\n",
                   transport->transport_id, transport->metrics_port);
        }

        sleep(5);  // 5-second interval
    }

    printf("[%s] Worker stopped\n", transport->transport_id);
    return NULL;
}

// Load transports from JSON config file
int load_transports_from_json() {
    FILE *fp = fopen(CONFIG_PATH, "r");
    if(!fp) {
        fprintf(stderr, "Failed to open config file: %s\n", CONFIG_PATH);
        return 0;
    }

    // Read entire file
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *json = malloc(fsize + 1);
    fread(json, 1, fsize, fp);
    fclose(fp);
    json[fsize] = 0;

    // Simple JSON parsing (looking for running transports)
    int count = 0;
    char *ptr = json;

    while((ptr = strstr(ptr, "\"id\"")) != NULL && count < MAX_TRANSPORTS) {
        // Find the start of this object (look backwards for opening brace)
        char *obj_start = ptr;
        while(obj_start > json && *obj_start != '{') obj_start--;

        // Find the end of this object (look forward for closing brace, accounting for nesting)
        char *obj_end = ptr;
        int brace_count = 0;
        while(*obj_end && (brace_count > 0 || *obj_end != '}')) {
            if(*obj_end == '{') brace_count++;
            if(*obj_end == '}') brace_count--;
            obj_end++;
        }

        // Extract ID
        ptr = strchr(ptr, ':');
        if(!ptr) break;
        ptr++;
        while(*ptr == ' ' || *ptr == '"') ptr++;
        char *id_end = strchr(ptr, '"');
        if(!id_end) break;

        int id_len = id_end - ptr;
        if(id_len >= sizeof(transports[count].transport_id)) {
            id_len = sizeof(transports[count].transport_id) - 1;
        }
        strncpy(transports[count].transport_id, ptr, id_len);
        transports[count].transport_id[id_len] = 0;

        // Look for metrics_port within this object
        char *metrics_ptr = obj_start;
        while(metrics_ptr < obj_end && (metrics_ptr = strstr(metrics_ptr, "\"metrics_port\"")) != NULL) {
            if(metrics_ptr > obj_end) break;
            metrics_ptr = strchr(metrics_ptr, ':');
            if(metrics_ptr && metrics_ptr < obj_end) {
                transports[count].metrics_port = atoi(metrics_ptr + 1);
                break;
            }
        }

        // Look for status within this object
        char *status_ptr = obj_start;
        while(status_ptr < obj_end && (status_ptr = strstr(status_ptr, "\"status\"")) != NULL) {
            if(status_ptr > obj_end) break;
            status_ptr = strchr(status_ptr, ':');
            if(status_ptr && status_ptr < obj_end && strstr(status_ptr, "running")) {
                transports[count].active = 1;
                count++;
                break;
            }
            status_ptr++;
        }

        ptr = obj_end + 1;
    }

    free(json);
    return count;
}

int main() {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  RIST Metrics Collector (C)           ║\n");
    printf("║  High Performance Metrics Collection  ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    // Connect to Redis
    printf("Connecting to Redis...\n");
    redis_ctx = redisConnect("127.0.0.1", 6379);
    if(redis_ctx == NULL || redis_ctx->err) {
        fprintf(stderr, "Redis connection error: %s\n",
                redis_ctx ? redis_ctx->errstr : "Can't allocate context");
        return 1;
    }

    // Test Redis connection
    redisReply *reply = redisCommand(redis_ctx, "PING");
    if(reply) {
        printf("Redis: %s\n", reply->str);
        freeReplyObject(reply);
    }

    // Load transports from config
    printf("\nLoading transports from: %s\n", CONFIG_PATH);
    memset(transports, 0, sizeof(transports));
    transport_count = load_transports_from_json();

    if(transport_count == 0) {
        printf("No running transports found. Waiting...\n");
    } else {
        printf("Found %d running transport(s):\n", transport_count);
        for(int i = 0; i < transport_count; i++) {
            printf("  - %s (port %d)\n",
                   transports[i].transport_id,
                   transports[i].metrics_port);
        }
        printf("\n");
    }

    // Start worker threads
    for(int i = 0; i < transport_count; i++) {
        pthread_create(&transports[i].thread, NULL,
                      transport_worker, &transports[i]);
    }

    printf("Collection started. Press Ctrl+C to stop.\n\n");

    // Main loop - reload transports periodically
    while(running) {
        sleep(60); // Check for new transports every minute

        // TODO: Reload transports from config and start/stop workers as needed
    }

    // Wait for all threads to finish
    printf("\nWaiting for workers to finish...\n");
    for(int i = 0; i < transport_count; i++) {
        transports[i].active = 0;
        pthread_join(transports[i].thread, NULL);
    }

    // Cleanup
    printf("Cleaning up...\n");
    if(redis_ctx) {
        redisFree(redis_ctx);
    }
    curl_global_cleanup();

    printf("Shutdown complete.\n");
    return 0;
}
