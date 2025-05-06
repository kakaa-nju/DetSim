#include "include/raft.h"
/* #include "include/raft_private.h" */
#include "../debug.h"
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/poll.h>
#include <malloc.h>
#include <sys/time.h>

#define NP 3
raft_server_t *raft;
int self;
raft_node_t nodes[NP];
const struct sockaddr_in addrs[NP] = {
  { .sin_family = AF_INET, .sin_addr.s_addr = 0x0100a8c0, .sin_port = 0x2823 },
  { .sin_family = AF_INET, .sin_addr.s_addr = 0x0200a8c0, .sin_port = 0x2823 },
  { .sin_family = AF_INET, .sin_addr.s_addr = 0x0300a8c0, .sin_port = 0x2823 }
};
int sockfd;
long msec_elapsed = 200;

enum {
  RAFT_MSG_APPENDENTRIES,
  RAFT_MSG_APPENDENTRIES_RESPONSE,
  RAFT_MSG_REQUESTVOTE,
  RAFT_MSG_REQUESTVOTE_RESPONSE,
  RAFT_MSG_ENTRY,
  RAFT_MSG_ENTRY_RESPONSE
};

char msg_buf[1024];

struct sockaddr_in getdest(void *node) {
  struct sockaddr_in dest;
  int i;
  for (i = 0; i < NP; i++) {
    if (node == nodes[i]) 
    {
      dest = addrs[i];
      break;
    }
  }
  assert(i != NP);
  return dest;
}

void *getnode(struct sockaddr_in *addr) {
  /* printf("%s:%d\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port)); */
  void *node;
  int i;
  for (i = 0; i < NP; i++) {
    if (!memcmp(addr, &addrs[i], sizeof(struct sockaddr_in)))
    {
      node = nodes[i];
      break;
    }
  }
  if (i == NP) {
    for (int j = 0; j < sizeof(struct sockaddr_in); j++) {
      printf("%02x ", ((unsigned char *)addr)[j]);
    }
    printf("\n");
    for (int j = 0; j < sizeof(struct sockaddr_in); j++) {
      printf("%02x ", ((unsigned char *)(addrs))[j]);
    }
    printf("\n");
  }
  fsync(1);
  assert(i != NP);
  return node;
}

int send_requestvote(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_requestvote_t *msg) {

  struct sockaddr_in dest = getdest(node);
  int type = RAFT_MSG_REQUESTVOTE;
  memcpy(msg_buf, &type, sizeof(int));
  memcpy(msg_buf + 4, msg, sizeof(*msg));
  memcpy(msg_buf + 4 + sizeof(*msg), &self, 4);

  sendto(sockfd, msg_buf, sizeof(msg_requestvote_t) + 8, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int send_requestvote_response(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_requestvote_response_t *msg) {
  struct sockaddr_in dest = getdest(node);
  int pos = 0;
  int type = RAFT_MSG_REQUESTVOTE_RESPONSE;
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);
  memcpy(msg_buf + pos, msg, sizeof(*msg));
  pos += sizeof(*msg);
  memcpy(msg_buf + pos, &self, 4);
  pos += 4;
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int send_appendentries_response(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_appendentries_response_t *msg) {
  struct sockaddr_in dest = getdest(node);
  int pos = 0;
  int type = RAFT_MSG_APPENDENTRIES_RESPONSE;
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);
  memcpy(msg_buf + pos, msg, sizeof(*msg));
  pos += sizeof(*msg);
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int send_appendentries(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_t *node,
    msg_appendentries_t *msg) {
  
  struct sockaddr_in dest = getdest(node);
  int pos = 0;
  int type = RAFT_MSG_APPENDENTRIES;
  memcpy(msg_buf, &type, sizeof(int));
  pos += sizeof(int);

  memcpy(msg_buf + pos, msg, (void *)&msg->entries - (void *)msg);
  pos += (void *)&msg->entries - (void *)msg;

  for (int i = 0; i < msg->n_entries; i++) 
  {
    raft_entry_t *entry = &msg->entries[i];
    raft_entry_data_t *data = &entry->data;

    memcpy(msg_buf + pos, entry, (void *)data - (void *)entry);
    pos += (void *)data - (void *)entry;
    memcpy(msg_buf + pos, &data->len, sizeof(data->len));
    pos += sizeof(data->len);
    memcpy(msg_buf + pos, data->buf, data->len);
    pos += data->len;
  }
  sendto(sockfd, msg_buf, pos, 0, (struct sockaddr *)&dest, sizeof(dest));
  return 0;
}

int applylog(
    raft_server_t *raft, 
    void *user_data, 
    raft_entry_t *entry,
    raft_index_t entry_idx) {
  // Implement applying log entry to the state machine here
  return 0;
}

int persist_vote(
    raft_server_t *raft, 
    void *user_data, 
    raft_node_id_t vote) {
  // Implement persisting voted node to disk here
  return 0;
}

int persist_term(
    raft_server_t *raft, 
    void *user_data, 
    raft_term_t term,
    raft_node_id_t vote) {
  // Implement persisting term and voted node to disk here
  return 0;
}

int log_offer(
    raft_server_t *raft, 
    void *user_data, 
    raft_entry_t *entry,
    raft_index_t entry_idx) {
  // Implement adding entry to the log here
  return 0;
}

int log_pop(
    raft_server_t *raft, 
    void *user_data, 
    raft_entry_t *entry,
    raft_index_t entry_idx) {
  // Implement removing youngest entry from the log here
  return 0;
}

void poll_msg() {
  struct sockaddr_in sender_addr = { 0 };
  socklen_t addr_len = sizeof(sender_addr);
  int recvlen = 0;
  
  while ((recvlen = recvfrom(sockfd, msg_buf, 1024, 0, (struct sockaddr *)&sender_addr, &addr_len)) > 0)
  {
    /* printf("recvfrom: %s:%d\n", inet_ntoa(sender_addr.sin_addr), ntohs(sender_addr.sin_port)); */
    void *sender = getnode(&sender_addr);
    int type;
    int pos = 0;
    memcpy(&type, msg_buf + pos, sizeof(type));
    pos += sizeof(type);
    switch (type)
    {
    case RAFT_MSG_APPENDENTRIES:
    {
      msg_appendentries_t *ae = malloc(sizeof(*ae));
      memcpy(ae, msg_buf + pos, (void *)&ae->entries - (void *)ae);
      pos += (void *)&ae->entries - (void *)ae;
      ae->entries = malloc(ae->n_entries * sizeof(msg_entry_t));
      for (int i = 0; i < ae->n_entries; i++) {
        raft_entry_t *entry = &ae->entries[i];
        memcpy(entry, msg_buf + pos, (void *)&entry->data - (void *)entry);
        pos += (void *)&entry->data - (void *)entry;
        memcpy(&entry->data.len, msg_buf + pos, sizeof(int));
        pos += sizeof(int);
        entry->data.buf = malloc(entry->data.len);
        memcpy(entry->data.buf, msg_buf + pos, entry->data.len);
        pos += entry->data.len;
      }
      
      msg_appendentries_response_t response;
      raft_recv_appendentries(raft, sender, ae, &response);
      /*
      __append_msg(me, &response, RAFT_MSG_APPENDENTRIES_RESPONSE,
                   sizeof(response), m->sender, me->raft);
                   */
      send_appendentries_response(raft, NULL, sender, &response);
      
    }
    break;
    case RAFT_MSG_APPENDENTRIES_RESPONSE:
    {
      msg_appendentries_response_t response;
      memcpy(&response, msg_buf + pos, sizeof(response));
      pos += sizeof(response);
      raft_recv_appendentries_response(raft, sender, &response);
      break;
    }
    case RAFT_MSG_REQUESTVOTE:
    {
      msg_requestvote_response_t response;
      msg_requestvote_t requestvote;
      memcpy(&requestvote, msg_buf + pos, sizeof(requestvote));
      pos += sizeof(requestvote);
      raft_recv_requestvote(raft, sender, &requestvote, &response);
      send_requestvote_response(raft, NULL, sender, &response);
      pos += 4;
      break;
    }
    case RAFT_MSG_REQUESTVOTE_RESPONSE: 
    {
      msg_requestvote_response_t response;
      memcpy(&response, msg_buf + pos, sizeof(response));
      pos += sizeof(response);
      raft_recv_requestvote_response(raft, sender, &response);
      pos += 4;
      break;
    }
      /*
    case RAFT_MSG_ENTRY:
    {
      msg_entry_response_t response;
      raft_recv_entry(me->raft, m->data, &response);
      __append_msg(me, &response, RAFT_MSG_ENTRY_RESPONSE,
                   sizeof(response), m->sender, me->raft);
    }
    break;

    case RAFT_MSG_ENTRY_RESPONSE:
      raft_recv_entry_response(me->raft, m->sender, m->data);
      break;
    */
    }
    assert(pos == recvlen);
  }
}

void logging(raft_server_t *s, raft_node_t *n, void *data, const char *buf) {
  puts(buf);
}

// Main function
int main(int argc, const char *argv[]) {
  /* addrs[0] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1"), .sin_port = htons(9900) }; */
  /* addrs[1] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1"), .sin_port = htons(9901) }; */
  /* addrs[2] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1"), .sin_port = htons(9902) }; */

  self = atoi(argv[1]);

  sockfd = socket(AF_INET, SOCK_DGRAM, 0); // only communication channel
  Assert(bind(sockfd, (struct sockaddr *)&addrs[self], sizeof(struct sockaddr_in)) == 0, "node %s", argv[1]);
  // Initialize Raft server
  raft = raft_new();
  /* ((raft_server_private_t *)raft)->state = RAFT_STATE_LEADER; */

  // Set callbacks
  raft_cbs_t callbacks = {.send_requestvote = send_requestvote,
                          .send_appendentries = send_appendentries,
                          .applylog = applylog,
                          .persist_vote = persist_vote,
                          .persist_term = persist_term,
                          .log_offer = log_offer,
                          .log_pop = log_pop,
                          /* .log = logging */
  };
  raft_set_callbacks(raft, &callbacks, NULL);
  raft_set_election_timeout(raft, 1000);

  // Add nodes
  nodes[0] = raft_add_node(raft, (void *)&addrs[0], 0, 0 == self);
  nodes[1] = raft_add_node(raft, (void *)&addrs[1], 1, 1 == self);
  nodes[2] = raft_add_node(raft, (void *)&addrs[2], 2, 2 == self);

  if (self == 0) 
  {
    /* sendto(sockfd, "hello", 5, 0, (struct sockaddr *)&addrs[1], sizeof(addrs[1])); */
    raft_periodic(raft, 2000);
  }

  struct timeval tv_old = {.tv_sec = 0, .tv_usec = 0};
  while (1)
  {
    struct timeval tv_new;
    gettimeofday(&tv_new, NULL);
    raft_periodic(raft, ((tv_new.tv_sec - tv_old.tv_sec) * 1000000 + (tv_new.tv_usec - tv_old.tv_usec)) / 1000);
    tv_old = tv_new;
    poll_msg();
  }

  raft_free(raft);

  return 0;
}
