#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <string.h>
#include <time.h>
#include <linux/ppdev.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define RINGER_RESET_TIME 15
#define RINGER_STATUS_BIT 0x10
#define BUZZER_ENABLE_DATA_BYTE 0xFF
#define BUZZER_DISABLE_DATA_BYTE 0x00
#define BUZZER_SOLENOID_REST_TIME 10
#define BUZZER_ON_TIME 1
#define SERVER_UDP_PORT 30012
#define MAXIMUM_SUBSCRIPTION_TIME 60
// Should end up being ~1K on most systems
#define MAXIMUM_CLIENT_SUBSCRIPTIONS 64

// Number of seconds to sleep in the main loop between iterations.
#define MAIN_LOOP_SLEEP_TIME 100000
#define SELECT_TIMEOUT 100000

//#define DEBUG
#define DAEMON

// Some strings that represent the tokens for the simple protocol.
//
// q_XXXXX -- a query / command
// r_XXXXX -- a reply
//
// q_getstatus -> r_null | r_ringing
// q_opengate -> r_acknowledged | r_already_opened
const char q_getstatus[] = "Sup?";
const char r_null[] = "Nothing.\n";
const char r_ringing[] = "RING!\n";

const char q_opengate[] = "OPEN!";
const char r_acknowledged[] = "Acknowledged. Buzzing it open.\n";
const char r_already_opened[] = "Already opened recently.\n";

const char q_subscribe[] = "subscribe ";
const char r_subscribe_success[] = "Ok, I'll keep you posted for up to MAXIMUM_SUBSCRIPTION_TIME seconds.\n";

const char r_error[] = "Internal error.\n";

// Represents if the ringer (call to get in) is ringing or has been recently rung.
unsigned short ringer_state = 0;
// Represents if the buzzer is ringing or has been recently buzzed.
unsigned short buzzer_state = 0;

// Represents the last time that the ringer was last detected to be ringing.
struct timeval last_ring_detected;
// Represents the last time that we fired the solenoid to be on.
struct timeval last_buzzer_firing, last_buzzer_request;

// FD to connect to the parallel port device.
int parport_file_descriptor;
// Defined in the global scope, as other functions will need this.
int listen_file_descriptor;

// Some structure to keep track of interested receivers.
// A "subscription" describes the time last described, and a sockaddr for the interested client.
// A "subscriptions" is a doubly-linked list of "subscription" structs.
struct subscription {
  time_t time;
  struct sockaddr_in client;
};
struct subscription_node {
  struct subscription_node* previous;
  struct subscription subscription;
  struct subscription_node* next;
};

struct subscription_node* subscriptions_head = NULL;

struct subscription* make_subscription(struct sockaddr_in* client) {
  struct subscription* subscription = (struct subscription*)malloc(sizeof(struct subscription));
  struct timeval now;
  gettimeofday(&now, NULL);
  subscription->time = now.tv_sec;
  subscription->client = *client;
  return(subscription);
}

struct subscription_node* make_subscription_node(struct subscription* subscription) {
  struct subscription_node* n = (struct subscription_node*)malloc(sizeof(struct subscription_node));
  n->previous = NULL;
  n->subscription = *subscription;
  n->next = NULL;
  return(n);
}

void insert_subscription_node(struct subscription_node* node) {
  struct subscription_node* n;
  if (subscriptions_head == NULL) { // A new, empty list
    node->previous = NULL;
    node->next = NULL;
    subscriptions_head = node;
  } else {
    // Walk the list, find the end, re-point pointers there
    for(n = subscriptions_head; n != NULL; n = n->next) {
      if(n->next == NULL) { // We're at the end of the list.
        node->previous = n;
        node->next = NULL;
        n->next = node;
        break;
      }
    }
  }
}

// Remove a node from the list by re-pointing it's neighbors, or the head
// pointer
void remove_subscription_node(struct subscription_node* node){
  struct subscription_node *previous, *next;
  previous = node->previous;
  next = node->next;

  if (previous == NULL && next == NULL) { // We're deleting the only node in the list.
    subscriptions_head = NULL;
  } else {
    if (previous != NULL) {
      previous->next = next;
    }
    if (next != NULL) {
      next->previous = previous;
    }
  }
  free(node);
}

// Walk over the list of subscriptions and return a pointer to a node with the
// client sockaddr being searched for.
// Will return a NULL pointer if not found. Hack?
struct subscription_node* find_subscription_node(struct sockaddr_in* client) {
  struct subscription_node* n;
  for(n = subscriptions_head; n != NULL; n = n->next) {
    if (n->subscription.client.sin_addr.s_addr == client->sin_addr.s_addr &&
        n->subscription.client.sin_port == client->sin_port) {
      // We found the first matching client subscription
      return(n);
    }
  }
  return(NULL); // No node found.
}

// Add or update a client's subscription to ringer state changes in the
// doubly-linked "subscriptions" list.
void subscribe_client(struct sockaddr_in* client) {
  struct subscription_node* subscription_node = find_subscription_node(client);
  if(subscription_node == NULL) { // This client is not already in the list.
    struct subscription_node* node = make_subscription_node(make_subscription(client));
    insert_subscription_node(node);
  } else { // The client is already in the list, update expiry time.
    struct timeval now;
    gettimeofday(&now, NULL);
    subscription_node->subscription.time = now.tv_sec;
  }
}

void subscribe_broadcast() {
  struct sockaddr_in* sa_broadcast = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
  sa_broadcast->sin_family = AF_INET;
  sa_broadcast->sin_port = htons(SERVER_UDP_PORT);
  sa_broadcast->sin_addr.s_addr = INADDR_BROADCAST;
  subscribe_client(sa_broadcast);
}

// Remove any subscriptions that have expired.
void purge_expired_subscriptions() {
  struct timeval now;
  gettimeofday(&now, NULL);
  time_t now_seconds = now.tv_sec;

  struct subscription_node* n;
  for (n = subscriptions_head; n != NULL; n = n->next) {
    if (now_seconds > ((n->subscription.time)+(MAXIMUM_SUBSCRIPTION_TIME))) { // Subscription has expired
      remove_subscription_node(n);
    }
  }
}

// send_response fires off a UDP packet.
int send_response(int socket_descriptor, struct sockaddr *destination_addr, socklen_t destination_addr_size, const char *message, size_t message_length) {
  ssize_t bytes_sent;
  bytes_sent = sendto(socket_descriptor, message, message_length-1, 0, (struct sockaddr *)destination_addr, destination_addr_size);
#ifdef DEBUG
  fprintf(stderr, "Sent \"%.*s\"\n", (int)message_length, message);
#endif
  if ( bytes_sent != (ssize_t)message_length ) {
    errno = ECOMM;
    perror("Error in sending response: ");
    return(-1);
  } else {
    return(0);
  }
}

// Fire off ringer event messages to anyone with subscriptions
void update_ringer_subscriptions() {
  struct subscription_node* n;
  for (n = subscriptions_head; n != NULL; n = n->next) {
    send_response(listen_file_descriptor, (struct sockaddr *)&(n->subscription.client), sizeof(n->subscription.client), r_ringing, sizeof(r_ringing));
  }
}

// Check to see if the ringer call button is currently depressed.
int is_buzzer_ringing(void) {
  int result;
  unsigned char parport_status_register;

  result = ioctl(parport_file_descriptor, PPRSTATUS, &parport_status_register);

  if (result < 0) {
    return -1;
  } else if ( (parport_status_register & RINGER_STATUS_BIT) == 0) { // 0 here, as the bit goes to 0 when the ringer is fired
    return 1;
  } else {
    return 0;
  }
}

// Make sure global variables cache the state of reality.
void update_ringer_state(void) {
  // buzzed? and buzzed age expired?
  //  clear buzzed state
  // not buzzed?
  //  check for buzzing, if so
  //   set buzzed state
  //   set last buzzed time
  struct timeval now;
  gettimeofday(&now, NULL);
  int time_delta = (int)now.tv_sec - (int)last_ring_detected.tv_sec;
  int result;
  result = is_buzzer_ringing();

  if ( ringer_state == 1 && time_delta >= RINGER_RESET_TIME ) {
    ringer_state = 0;
#ifdef DEBUG
    fprintf(stderr, "ringer_state clearing...\n");
#endif
  } else if ( ringer_state == 0 && result == 1 ) {
#ifdef DEBUG
    fprintf(stderr, "ringer_state is getting set. We're ringing.\n");
#endif
    ringer_state = 1;
    last_ring_detected = now;
  }
}

int write_parport_data_register(unsigned char data_register) {
  int result;

  struct ppdev_frob_struct frob;
  frob.mask = 0x02;
  frob.val = 0x02;
  result = ioctl(parport_file_descriptor, PPFCONTROL, &frob);

  result = ioctl(parport_file_descriptor, PPWDATA, &data_register);

#ifdef DEBUG
  fprintf(stderr, "Tried to write %02X to parallel port data register. Result was %02d\n", data_register, result);
  unsigned char data_after;
  result = ioctl(parport_file_descriptor, PPRDATA, &data_after);
  fprintf(stderr, "Read %02X, result was %02d\n", data_after, result);
#endif

  frob.mask = 0x02;
  frob.val = 0x02;
  result = ioctl(parport_file_descriptor, PPFCONTROL, &frob);

  return(result);
}

// Set the data register to enable the solenoid.
int enable_buzzer_solenoid(void) {
#ifdef DEBUG
  fprintf(stderr, "Trying to enable solenoid.\n");
#endif
  int result;
  result = write_parport_data_register(BUZZER_ENABLE_DATA_BYTE);
  if (result < 0) {
    perror("Error writing data register to parallel port while enabling solenoid: ");
  }
  return(result);
}
int disable_buzzer_solenoid(void) {
#ifdef DEBUG
  fprintf(stderr, "Trying to disable solenoid.\n");
#endif
  int result;
  result = write_parport_data_register(BUZZER_DISABLE_DATA_BYTE);
  if (result < 0) {
    perror("Error writing data register to parallel port while disabling solenoid: ");
  }
  return(result);
}

int update_buzzer_state(void) {
  // if buzzer_state
  //   if timedelta(now, last_buzzer_firing) > BUZZER_ON_TIME
  //    disable buzzer
  //    disable state
  int result;
  struct timeval now;
  gettimeofday(&now, NULL);
  int time_delta = (int)now.tv_sec - (int)last_buzzer_firing.tv_sec;

  if ( buzzer_state == 1 && time_delta > BUZZER_ON_TIME ) {
    result = disable_buzzer_solenoid();
    buzzer_state = 0;
  } else {
    result = 0;
  }
  return(result);
}


// Buzz open the gate, but not too much.
int buzz_open_gate(void) {
  // if opened less than BUZZER_SOLENOID_REST_TIME, or buzzer_state == 1 already, return 1 (already opened)
  // otherwise, open the gate and return 0 (success)
  // This may also return -1 from the underlying calls to ioctl(2) in enable_buzzer_solenoid
  int result;
  struct timeval now;
  gettimeofday(&now, NULL);
  int time_delta = (int)now.tv_sec - (int)last_buzzer_firing.tv_sec;

  if (buzzer_state == 1 || time_delta < BUZZER_SOLENOID_REST_TIME) {
    return(1);
  } else {
    result = enable_buzzer_solenoid();
    buzzer_state = 1;
    last_buzzer_firing = now;
    return(result);
  }
}

int main() {
  int listen_file_descriptor, result;
  struct sockaddr_in server_address, client_address;
  useconds_t sleeptime = MAIN_LOOP_SLEEP_TIME;
  ssize_t bytes_received;
  socklen_t client_struct_length = sizeof(client_address);
  fd_set read_file_descriptors;
  struct timeval select_timeout = {
    .tv_sec = 0,
    .tv_usec = SELECT_TIMEOUT
  };

#ifdef DAEMON
  int i;
  for (i = getdtablesize(); i>=0; --i) {
    close(i);
  }
  i = fork();
  if (i < 0) {
    perror("Error in forking.");
    exit(1);
  } else if (i > 0) {
    // Parent
    exit(0);
  }
  // Get a new process group.
  setsid();
#endif



  // Open up the parallel port
  parport_file_descriptor = open("/dev/parport0", O_RDWR);
  if (parport_file_descriptor < 0) {
    perror("Error in opening /dev/parport0: ");
    exit(1);
  }
  // Seize control of it
  result = ioctl(parport_file_descriptor, PPCLAIM);
  if (result < 0) {
    perror("Error in claiming parallel port: ");
    exit(1);
  }

  // Start up a server UDP socket, and begin listening.
  listen_file_descriptor = socket(AF_INET, SOCK_DGRAM, 0);
  if (listen_file_descriptor < 0) {
    perror("Error in opening socket: ");
    exit(1);
  }

  int current_flags;
  current_flags = fcntl(listen_file_descriptor, F_GETFL);
  result = fcntl(listen_file_descriptor, F_SETFL, current_flags | O_NONBLOCK);
  if (result < 0) {
    perror("Error in setting socket to be non-blocking: ");
    exit(1);
  }

  bzero(&server_address, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(SERVER_UDP_PORT);
  result = bind(listen_file_descriptor, (struct sockaddr *)&server_address, sizeof(server_address));
  if ( result < 0 ) {
    perror("Error in binding a server socket: ");
    exit(1);
  }

  for(;;) {
    // Update ringer state
    update_ringer_state();
    update_buzzer_state();

    // get ready for reception
    char command_buffer[255];
    bzero(&command_buffer, sizeof(command_buffer));

    // receive a command packet, if there is one
    select_timeout.tv_sec = 0;
    select_timeout.tv_usec = SELECT_TIMEOUT;
    FD_ZERO(&read_file_descriptors);
    FD_SET(listen_file_descriptor, &read_file_descriptors);
    result = select(FD_SETSIZE, &read_file_descriptors, NULL, NULL, &select_timeout);

    if (result > 0) {
      bytes_received = recvfrom(listen_file_descriptor, &command_buffer, sizeof(command_buffer), MSG_DONTWAIT, (struct sockaddr *)&client_address, &client_struct_length);

      if (bytes_received > 0) { // we got something, handle it
#ifdef DEBUG
        fprintf(stderr, "Received \"%.*s\"\n", (int)bytes_received, command_buffer);
#endif
        // Compare the largest command first. if/elses at this level ought to be sorted by size. There ought to be a better way.
        if ( (strncmp(q_subscribe, command_buffer, sizeof(q_subscribe))) == 0 ) {
          // Try and subscribe the remote client to ringer updates. r_subscribe_success or r_subscribe_error_too_long in response.
          subscribe_client(&client_address);
          send_response(listen_file_descriptor, (struct sockaddr *)&client_address, client_struct_length, r_subscribe_success, sizeof(r_subscribe_success));
        } else if ( (strncmp(q_opengate, command_buffer, sizeof(q_opengate))) == 0 ) { 
          // try and open the gate, r_acknowledged or r_already_opened in response
          result = buzz_open_gate();
          if (result == 0) {
            send_response(listen_file_descriptor, (struct sockaddr *)&client_address, client_struct_length, r_acknowledged, sizeof(r_acknowledged));
          } else if (result == 1) {
            send_response(listen_file_descriptor, (struct sockaddr *)&client_address, client_struct_length, r_already_opened, sizeof(r_already_opened));
          } else if (result == 1) {
            send_response(listen_file_descriptor, (struct sockaddr *)&client_address, client_struct_length, r_error, sizeof(r_error));
          }
        } else if ( (strncmp(q_getstatus, command_buffer, sizeof(q_getstatus))) == 0) {
          // see if we've recently been rung. if so, r_ringing, else r_null
          if (ringer_state == 1) {
            send_response(listen_file_descriptor, (struct sockaddr *)&client_address, client_struct_length, r_ringing, sizeof(r_ringing));
          } else {
            send_response(listen_file_descriptor, (struct sockaddr *)&client_address, client_struct_length, r_null, sizeof(r_null));
          }
        }
      } 
    }

    usleep(sleeptime);
  } // end of main for loop


  return(0);
}
