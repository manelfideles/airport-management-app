#ifndef STRUCTS_PROJETO_H
#define STRUCTS_PROJETO_H

#define BUF_SIZE 100

typedef struct {
  int ut;      //unidade de tempo em milissegundos
  int T, dt;    //duracao da descolagem(em ut), intervalo entre descolagens(em ut)
  int L, dl;    //duracao da aterragem(em ut), intervalo entre aterragens(em ut)
  int min, max; //holding minimo e holding maximo
  int D;       //qtd maxima de partidas no sistema
  int A;       //qtd maxima de chegadas no sistema
} InitialConfig;

typedef struct {
  char flight_code[50];
  int init, takeoff_time;
} departing_flight;

typedef struct {
  char flight_code[50];
  int init, eta, fuel;
} landing_flight;

typedef struct {
  int total_flights_created;
  int n_landings;
  int n_departures;
  double awt_departure; //awt == average waiting time
  double awt_landing;
  double avg_holdings_per_landing;
  double avg_holdings_per_emergency_landing;
  int n_diverted_flights;
  int n_rejected_flights;
} Stats;

//LL in controlTower
typedef struct departing_flight_list_node {
  departing_flight flight;
  struct departing_flight_list_node *next;
} DepartingListNode;

//LL in controlTower
typedef struct landing_flight_list_node {
  landing_flight flight;
  struct landing_flight_list_node *next;
} LandingListNode;

//Message
typedef struct landing_flight_msg {
  long mtype;
  int eta, fuel, init;
} LandingFlightMsg;

typedef struct departing_flight_msg {
  long mtype;
  int takeoff_time, init;
} DepartingFlightMsg;

typedef struct mem_slot_msg {
  long mtype;
  int slot;
  int init;
} MemSlotMsg;

typedef struct runway {
  char id[10];
  int is_occupied;
  int time_left_until_free;
} Runway;

typedef struct {
  int command;
  Runway *runway;
} Command;

typedef struct ready_to_land {
  int init, initial_fuel;
  int eta, fuel;
  int slot;
  struct ready_to_land *next;
} ReadyToLand;

typedef struct ready_to_fly {
  int init;
  int takeoff_time;
  int slot;
  struct ready_to_fly *next;
} ReadyToFly;

#endif /*STRUCTS_PROJETO_H*/
