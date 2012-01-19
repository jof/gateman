#include <errno.h>
#include <stdio.h>
#include <string.h>

// Ardunio-specific headers
#include <Ethernet.h>
#include <Udp.h>
#include <SPI.h>

#include <stdarg.h>
void p(char *fmt, ... ){
        char tmp[128]; // resulting string limited to 128 chars
        va_list args;
        va_start (args, fmt );
        vsnprintf(tmp, 128, fmt, args);
        va_end (args);
        Serial.print(tmp);
}

// FIXME: times need to be converted from seconds to "millis", currently every
//   instance in the code scales up second values into millisecond values.
#define RINGER_RESET_TIME 5
#define BUZZER_SOLENOID_REST_TIME 10
#define BUZZER_ON_TIME 1
#define SERVER_UDP_PORT 30012
#define RINGER_PIN 3 // Interrupt "1"
#define RINGER_INTERRUPT 1
#define BUZZER_PIN 5
#define ON 1
#define OFF 0

#define DEBUG

// Some strings that represent the tokens for the simple protocol.
//
// q_XXXXX -- a query / command
// r_XXXXX -- a reply
//
// q_getstatus -> r_null | r_ringing
// q_opengate -> r_acknowledged | r_already_opened
const char q_getstatus[] = "Sup?";
const char r_null[] = "Nothing.";
const char r_ringing[] = "RING!";

const char q_opengate[] = "OPEN!";
const char r_acknowledged[] = "Acknowledged. Buzzing it open.";
const char r_already_opened[] = "Already opened recently.";

const char r_badrequest[] = "Huh?";

// Represents the last time that we fired the solenoid to be on.
unsigned long last_buzzer_firing;
// Last time we got an interrupt from the ringer being pushed
unsigned long last_ring_detected;

// Represents if the ringer (call to get in) is ringing or has been recently rung.
volatile unsigned short ringer_state = 0;
// Represents if the buzzer (to allow the gate to open) is ringing or has been recently buzzed.
unsigned short buzzer_state = 0;
// Our IPv4 IP
byte ip[] = { 172, 30, 0, 21 };
// Our Ethernet MAC address
byte mac[] = { 0xDE, 0xAD, 0x00, 0x0D, 0x00, 0x12 };

// UDP_TX_PACKET_MAX_SIZE is defined in Ethernet/Udp.h as 24 bytes. Why?
char packet_buffer[UDP_TX_PACKET_MAX_SIZE]; 

// Places to store who we're talking to.
uint8_t remote_ip[4];
unsigned int remote_port;

uint16_t send_response(const char *message, uint8_t *destination_ip, uint16_t destination_port) {
  uint16_t bytes_sent;
  bytes_sent = Udp.sendPacket((const char *)message, destination_ip, destination_port);
  return(bytes_sent);
}

// Buzz open the gate, but not too much.
short buzz_open_gate(void) {
  // if opened less than BUZZER_SOLENOID_REST_TIME, or buzzer_state == 1, return 1
  // otherwise, open the gate and return 0
  unsigned long now = millis();
  unsigned long time_delta = now - last_buzzer_firing;

  if (buzzer_state == ON || time_delta < BUZZER_SOLENOID_REST_TIME) {
    // Already opened recently.
    return(-1);
  } else {
    // Begin buzzing the door system
    digitalWrite(BUZZER_PIN, HIGH);
    buzzer_state = ON;
    last_buzzer_firing = now;
    return(0);
  }
}

// Reset the ringer state if it's already been set long enough, unless the
// ringer is still being rung.
void update_ringer_state(void) {
  if (digitalRead(RINGER_PIN) == LOW) { // Ringing now
#ifdef DEBUG
    if (millis() % 750 == 0) {
      p("Ringing detected.\n");
    }
#endif
    ringer_state = 1;
    last_ring_detected = millis();
  } else { // Possibly stopped ringing
    unsigned long time_delta = millis() - last_ring_detected;
    if ( ringer_state == 1 && time_delta >= ((RINGER_RESET_TIME)*1000) ) {
      ringer_state = 0;
    }
  }
}

// An interrupt handler called in setup() that should keep "ringer_state" fresh.
void ringer_isr() {
#ifdef DEBUG
  p("Ringing detected via interrupt.\n");
#endif
  update_ringer_state();
}

void setup(void) {
  Ethernet.begin(mac, ip);
  Udp.begin(SERVER_UDP_PORT);
  Serial.begin(9600);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RINGER_PIN, INPUT);
  // Enable intrnal pullups on ringer pin.
  digitalWrite(RINGER_PIN, HIGH);

  // FIXME: how will the optoisolator look to this? ringing == LOW? HIGH?
  attachInterrupt(RINGER_INTERRUPT, ringer_isr, FALLING);
}

void loop(void) {
  // Shut off the buzzer if it's already been "on" long enough
  // FIXME: for the delta, now could possibly rollover in ~50 days. Problem? U mad bro?
  unsigned long time_delta = millis() - last_buzzer_firing;
  if ( buzzer_state == ON && time_delta > BUZZER_ON_TIME ) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzer_state = 0;
  }

  update_ringer_state();

  int udp_packet_size;
  int ip_packet_size = Udp.available();
  if (ip_packet_size) { // If we've received a packet at all, handle it.
    udp_packet_size = ip_packet_size - 8;      // Less a UDP header

#ifdef DEBUG
    Serial.print("Got a ");
    Serial.print(ip_packet_size);
    Serial.print("-byte IP packet ");
#endif

    // Reset and NUL out the packet payload buffer.
    memset(&packet_buffer, 0, sizeof(packet_buffer));

    // read the packet into packetBufffer and get the senders IP addr and port number
    Udp.readPacket(packet_buffer, UDP_TX_PACKET_MAX_SIZE, remote_ip, remote_port);
#ifdef DEBUG
    //FIXME, TODO: get printf working with STDIO redirection to UART. Add debugging info.
    p(" from %u.%u.%u.%u[%u] ", remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3], remote_port);
    p(" containing: %s\n", packet_buffer);
#endif

    short result;
    // comparre the largest command first. if/elses at this level need to be sorted by size. There ought to be a better way.
    if ( (strncmp(q_opengate, packet_buffer, sizeof(q_opengate))) == 0 ) { 
      // try and open the gate, r_acknowledged or r_already_opened in response
      result = buzz_open_gate();
      if (result == 0) {
        send_response(r_acknowledged, remote_ip, remote_port);
      } else if (result == -1) {
        send_response(r_already_opened, remote_ip, remote_port);
      }
    } else if ( (strncmp(q_getstatus, packet_buffer, sizeof(q_getstatus))) == 0) {
      // see if we've recently been rung. if so, r_ringing, else r_null
      if (ringer_state == 1) {
        send_response(r_ringing, remote_ip, remote_port);
      } else {
        send_response(r_null, remote_ip, remote_port);
      }
    } else {
      send_response(r_badrequest, remote_ip, remote_port);
    }
  } 
}
