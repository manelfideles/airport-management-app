
/*
Manuel Fideles [2018282990]
Guilherme Gaspar [2018278327]
*/

/*
Funcionamento geral do programa:
Os comandos que entram são recebidos através de um 'input_pipe'.
De seguida, o programa verifica se os mesmos são válidos. Se forem,
preenche uma struct 'voo' (departing_flight/landing_flight)
e insere essa struct na respetiva lista ligada.
Quando o init de uma dada struct é atingido, uma thread 'voo'
arranca. Essa thread envia, através da MSQ, a sua
informação para a CT (fuel e eta, no caso de uma aterragem,
takeoff_time no caso de uma descolagem).
A CT verifica se há espaço livre na memória partilhada. Se houver,
devolve à thread o slot de memória que lhe foi designado - e vai ser
por aí que a comunicação entre a CT e a thread 'voo' se irá dar.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <semaphore.h>
#include <fcntl.h>
#include <ctype.h>
#include "structs_projeto.h"

//NAMES
//files
#define CONFIG_FILE "config.txt"
#define LOG "log.txt"
#define PIPE_NAME "/tmp/pipe"

//mutexes/semaphores
#define MUTEX_SHM "mutex_shm"
#define RECEIVED_DEPARTURE "received_departure"
#define RECEIVED_LANDING "received_landing"
#define LANDING_LIST_MUTEX "landing_flight_list_mutex"
#define DEPARTURE_LIST_MUTEX "departing_flight_list_mutex"
#define MUTEX_MSQ "mutex_msq"
#define STATUS_UPDATE_ARRIVALS "status_update_arrivals"
#define STATUS_UPDATE_DEPARTURES "status_update_departures"
#define TIMER "timer"

//buffer size
#define BUF_SIZE 100

//message types
#define DEPARTURE_TYPE 1
#define ARRIVAL_TYPE 2

//shm slot states
#define OCCUPIED 1
#define FREE 0
#define EMERGENCY 2
#define REDIRECT 3
#define HOLDING 4
#define REQUESTING_PERMISSION_FOR_LANDING 5
#define LANDING 6
#define DEPARTURE 7
#define WAIT 8

//config file variables
#define TIME_UNITS (configs.ut * 0.001) //in seconds
#define SHM_SIZE (configs.D + configs.A)

/*----- GLOBAL VARIABLES -----*/

//Structs
InitialConfig configs;
Stats stats;

//msq
int msq;
pthread_t receive_departures;
pthread_t receive_landings;

//shared memory
int shmid;
Command * shm;

//program start time
time_t init_time;
pthread_t time_counter;

//initializing 4 runways
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex3 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex4 = PTHREAD_MUTEX_INITIALIZER;

Runway l1;
Runway l2;
Runway d1;
Runway d2;

//mutex for msq
sem_t *mutex_msq;
sem_t *timer;
int ut = 0;

//mutex for shm accesses
sem_t *mutex_shm;

//semaphors that verify if a new CT command was given
sem_t *status_update_arrivals;
sem_t *status_update_departures;

//semaphores to check for messages
sem_t *received_departure;
sem_t *received_landing;

//CT waiting list mutexes
sem_t *landing_flight_list_mutex;
sem_t *departing_flight_list_mutex;

//pipe fd
int input_pipe_fd;

//SimManager's pid
pid_t sm_pid;

//waiting lists @ SM - utilized to init threads at the right time
DepartingListNode * departure_list_head = NULL;
LandingListNode * landing_list_head = NULL;

//waiting lists @ CT
ReadyToLand * ready_to_land_list_head = NULL;
ReadyToFly * ready_to_fly_list_head = NULL;

//Flight-creating threads @ SM
pthread_t departures_list_thread;
pthread_t landings_list_thread;

/*----- ROUTINES -----*/

char* printTime() {
  /*
  Prints system time.
  */
  time_t rawtime;
  struct tm *info;
  char buffer[80];
  char *bufferp = buffer;
  time(&rawtime);
  info = localtime(&rawtime);
  strftime(buffer, 79, "%I:%M:%S", info);
  return bufferp;
}
void countTime() {
  /*
  Increments the program's time units
  */
  while(1) {
    usleep(configs.ut * 1000);
    printf("Time: %d\n", ut++);
    sem_post(timer);
  }
}
void cleanup() {
  /*
  Upon Ctrl+C, this function
  kills all ipc resources, flight Threads,
  the SM and CT processes, frees lists
  */

  printf("Total flights created: %d\n", stats.total_flights_created);
  printf("Landings: %d\n", stats.n_landings);
  printf("Departures: %d\n", stats.n_departures);
  printf("Rejections: %d\n", stats.n_rejected_flights);
  printf("Redirected flights: %d\n", stats.n_diverted_flights);
  printf("Average waiting time (Departures): %.2f\n", stats.awt_departure);
  printf("Average waiting time (Landings): %.2f\n", stats.awt_landing);
  printf("Average holding time per landings: %.2f\n", stats.avg_holdings_per_landing);
  printf("Average holding time per emergency landing: %.2f\n", stats.avg_holdings_per_emergency_landing);

  printf("\n[%d] Terminating...\n\n", getpid());

  // pthread_join(departures_list_thread, NULL);
  // pthread_join(landings_list_thread, NULL);
  printf("Threads terminated.\n");

  //free LL of flights
  ReadyToLand *x;
  for(x = ready_to_land_list_head; x != NULL; x = x->next) {
    free(x);
  }
  printf("Freed ReadyToLand list.\n");

  ReadyToFly *j;
  for(j = ready_to_fly_list_head; j != NULL; j = j->next) {
    free(j);
  }
  printf("Freed ReadyToFly list.\n");
  wait(NULL);

  if(getpid() == sm_pid) {
    //kill SHM
    shmdt(shm);
    shmctl(shmid, IPC_RMID, NULL);
    printf("Shared memory eliminated.\n");

    //destroy semaphores
    pthread_mutex_destroy(&mutex1);
    pthread_mutex_destroy(&mutex2);
    pthread_mutex_destroy(&mutex3);
    pthread_mutex_destroy(&mutex4);
    sem_unlink(MUTEX_SHM);
    sem_unlink(MUTEX_MSQ);
    sem_unlink(LANDING_LIST_MUTEX);
    sem_unlink(DEPARTURE_LIST_MUTEX);
    sem_unlink(RECEIVED_DEPARTURE);
    sem_unlink(RECEIVED_LANDING);
    printf("All mutexes/semaphores destroyed.\n");

    //kill msq
    if(msgctl(msq, IPC_RMID, NULL) != 0) {
      perror("msgctl error: ");
      exit(1);
    }
    printf("Message queue terminated.\n");
  }

  //close pipe
  close(input_pipe_fd);
  fflush(stdout);
  unlink(PIPE_NAME);
  printf("Pipe closed and eliminated.\n");

  exit(0);
}
DepartingListNode* newDepartingListNode(char **info_vector) {
  /*
  Creates a new node @ SM.
  Fills it with flight info.
  */
  DepartingListNode* new_node = malloc(sizeof(DepartingListNode));
  if (new_node == NULL) {
    perror("[newDepartingListNode] malloc error: ");
    exit(1);
  }
  strcpy(new_node->flight.flight_code, info_vector[1]);
  new_node->flight.init = atoi(info_vector[3]);
  new_node->flight.takeoff_time = atoi(info_vector[5]);
  return new_node;
}
LandingListNode* newLandingListNode(char **info_vector) {
  /*
  Creates a new node @ SM.
  Fills it with flight info.
  */
  LandingListNode* new_node = malloc(sizeof(LandingListNode));
  if (new_node == NULL) {
    perror("[newLandingListNode] malloc error: ");
    exit(1);
  }
  strcpy(new_node->flight.flight_code, info_vector[1]);
  new_node->flight.init = atoi(info_vector[3]);
  new_node->flight.eta = atoi(info_vector[5]);
  new_node->flight.fuel = atoi(info_vector[7]);
  return new_node;
}
ReadyToFly* newReadyToFlyNode(DepartingFlightMsg msg, MemSlotMsg slot) {
  /*
  Creates a new node @ CT.
  Fills it with information from
  the MSQ.
  */
  ReadyToFly * new_node = malloc(sizeof(ReadyToFly));
  if(new_node == NULL) {
    perror("[newReadyToFlyNode] malloc error");
    exit(1);
  }
  new_node->takeoff_time = msg.takeoff_time;
  new_node->slot = slot.slot;
  return new_node;
}
ReadyToLand* newReadyToLandNode(LandingFlightMsg msg, MemSlotMsg slot) {
  /*
  Creates a new node @ CT.
  Fills it with information from
  the MSQ.
  */
  ReadyToLand * new_node = malloc(sizeof(ReadyToLand));
  if(new_node == NULL) {
    perror("[newReadyToLandNode] malloc error");
    exit(1);
  }
  new_node->eta = msg.eta;
  new_node->fuel = msg.fuel;
  new_node->initial_fuel = msg.fuel;
  new_node->slot = slot.slot;
  new_node->init = msg.init;
  return new_node;
}
void printList(char type[]) {
  /*
  Prints flight queue @ SM.
  */
  if(strcmp(type, "ARRIVAL") == 0) {
    LandingListNode *i;
    printf("Arrival list:\n");
    for(i = landing_list_head; i != NULL; i = i->next) {
      printf("Flight #%s\n", i->flight.flight_code);
    }
    printf("-----\n");
  }
  else {
    DepartingListNode *j;
    printf("Departure List:\n");
    for(j = departure_list_head; j != NULL; j = j->next) {
      printf("Flight #%s\n", j->flight.flight_code);
    }
    printf("-----\n");
  }
}
void printReadyList(char type) {
  /*
  Prints queue @ CT.
  */
  if (type == 'D') {
    ReadyToFly *i;
    printf("ReadyToFly List:\n");
    for(i = ready_to_fly_list_head; i != NULL; i = i->next) {
      printf("(Slot, TAKEOFF) = (%d, %d)\n", i->slot, i->takeoff_time);
    }
  }
  else {
    ReadyToLand *j;
    printf("ReadyToLand List:\n");
    for(j = ready_to_land_list_head; j != NULL; j = j->next) {
      printf("(Slot, ETA) = (%d, %d)\n", j->slot, j->eta);
    }
  }
}
void sortedInsertDepartures(DepartingListNode **head, DepartingListNode *new_node) {
  /*
  Sorts and inserts new_node in the
  DepartingListNode list @ SM.
  */
  DepartingListNode *i;
  //Edge case for the head
  if(*head == NULL || (*head)->flight.init >= new_node->flight.init) {
    new_node->next = *head;
    *head = new_node;
  }
  else {
    //find the node just before the point of insertion
    i = *head;
    while((i->next != NULL) && i->next->flight.init < new_node->flight.init) {
      i = i->next;
    }
    new_node->next = i->next;
    i->next = new_node;
  }
}
void sortedInsertArrivals(LandingListNode **head, LandingListNode *new_node) {
  /*
  Sorts and inserts new_node in the
  DepartingListNode list @ SM.
  */
  LandingListNode *i;
  //Edge case for the head
  if(*head == NULL || (*head)->flight.init >= new_node->flight.init) {
    new_node->next = *head;
    *head = new_node;
  }
  else {
    //find the node just before the point of insertion
    i = *head;
    while((i->next != NULL) && i->next->flight.init < new_node->flight.init) {
      i = i->next;
    }
    new_node->next = i->next;
    i->next = new_node;
  }
}
void sortedInsertReadyDepartures(ReadyToFly **head, ReadyToFly *new_node) {
  ReadyToFly *i;
  //Edge case for the head
  if(*head == NULL || (*head)->takeoff_time >= new_node->takeoff_time) {
    new_node->next = *head;
    *head = new_node;
  }
  else {
    //find the node just before the point of insertion
    i = *head;
    while((i->next != NULL) && i->next->takeoff_time < new_node->takeoff_time) {
      i = i->next;
    }
    new_node->next = i->next;
    i->next = new_node;
  }
}
void sortedInsertReadyArrivals(ReadyToLand **head, ReadyToLand *new_node) {
  ReadyToLand *i;
  //Edge case for the head
  if(*head == NULL || (*head)->eta >= new_node->eta) {
    new_node->next = *head;
    *head = new_node;
  }
  else {
    //find the node just before the point of insertion
    i = *head;
    while((i->next != NULL) && i->next->eta < new_node->eta) {
      i = i->next;
    }
    new_node->next = i->next;
    i->next = new_node;
  }
}
int doesFlightAlreadyExist(char **info_vector) {
  /*
  Verifies if flight already exists
  inside respective LL queue @ SM.
  If it doesn't, it returns 0. Else, it
  returns 1.
  */
  if (strcmp(info_vector[0], "ARRIVAL") == 0) {
    int arrival_exists = 0;
    LandingListNode *i;
    for(i = landing_list_head; i != NULL; i = i->next) {
      if(strcmp(i->flight.flight_code, info_vector[1]) == 0) {
        arrival_exists = 1;
        break;
      }
    }
    return arrival_exists;
  }
  if (strcmp(info_vector[0], "DEPARTURE") == 0) {
    int departure_exists = 0;
    DepartingListNode *j;
    for(j = departure_list_head; j != NULL; j = j->next) {
      if(strcmp(j->flight.flight_code, info_vector[1]) == 0) {
        departure_exists = 1;
        break;
      }
    }
    return departure_exists;
  }
  return -1;
}
int isFlightUrgent(char **info_vector) {
  /*
  Checks if flight is urgent
  when the flight command is inputed.
  Returns 1 if it is, 0 if not urgent.
  */
  if(strcmp(info_vector[0], "ARRIVAL") == 0) {
    if(info_vector[7] == 4 + info_vector[5] + configs.L) return 1;
    else return 0;
  }
  return -1;
}
void writeFlightToLog(char c, void* flight, Runway *runway, char status[]) {
  /*
  Writes flight activity
  to the log.txt file
  */
  FILE * f = fopen(LOG, "a");
  if (f == NULL) {
    perror("Couldn't open 'log.txt'.");
    exit(1);
  }
  if (c == 'L') {
    LandingListNode l = *((LandingListNode *) flight);
    fprintf(f, "%s %s [%s] @ %s %s\n", printTime(), "ARRIVAL", l.flight.flight_code, runway->id, status);
  }
  else if (c == 'E') {
    LandingListNode l = *((LandingListNode *) flight);
    fprintf(f, "%s %s EMERGENCY LANDING REQUESTED\n", printTime(), l.flight.flight_code);
  }
  else if (c == 'D'){
    DepartingListNode d = *((DepartingListNode *) flight);
    fprintf(f, "%s %s [%s] @ %s %s\n", printTime(), "DEPARTURE", d.flight.flight_code, runway->id, status);
  }
  fclose(f);
}
Runway* checkForClearRunway(char c) {
  /*
  Checks if there's a clear runway for
  flight type 'c' - 'L' for landings,
  'D' for departures.
  Returns a free runway on success,
  and NULL if all tracks are taken.
  */
  if (c == 'L') {
    if(l1.is_occupied == 0) {
      return &l1;
    }
    else if(l2.is_occupied == 0) {
      return &l2;
    }
  }
  if (c == 'D') {
    if (d1.is_occupied == 0) {
      return &d1;
    }
    else if(d2.is_occupied == 0) {
      return &d2;
    }
  }
  return NULL;
}
void* landingFlight(void *flight) {
  /*
  Sends its flight info to CT
  and receives its designated memory slot.
  Updates stats and executes the command
  written to memory by the CT.
  */
  LandingListNode f = *((LandingListNode *) flight);

  //update stats
  stats.total_flights_created++;
  stats.n_landings++;

  //msgrcv
  MemSlotMsg slot;

  //msgsnd
  LandingFlightMsg arr_msg;
  long mtype = ARRIVAL_TYPE; //LANDING
  int eta = f.flight.eta;
  int fuel = f.flight.fuel;
  int init = f.flight.init;

  //Create message
  arr_msg.mtype = mtype;
  arr_msg.eta = eta;
  arr_msg.fuel = fuel;
  arr_msg.init = init;

  //Add to msq
  if(msgsnd(msq, &arr_msg, sizeof(LandingFlightMsg) - sizeof(long), 0) < 0) {
    perror("msgsnd error: ");
    exit(1);
  }

  sem_post(received_landing);
  sem_post(mutex_msq);

  //receive shm slot where the thread and CT will communicate
  if(msgrcv(msq, &slot, sizeof(MemSlotMsg) - sizeof(long), ARRIVAL_TYPE, 0) < 0) {
    perror("msgrcv: ");
    exit(1);
  }

  while(1) {
    sem_wait(mutex_shm);
    if(sem_trywait(status_update_arrivals) == 0) {
      switch (shm[slot.slot].command) {
        case LANDING: {
          printf("%s ARRIVAL [%s] @ %s started\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          writeFlightToLog('L', &f, checkForClearRunway('L'), "started");
          shm[slot.slot].runway->is_occupied = 1;
          sleep(configs.L * TIME_UNITS);
          shm[slot.slot].runway->is_occupied = 0;
          printf("%s ARRIVAL [%s] @ %s concluded\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          writeFlightToLog('L', &f, checkForClearRunway('L'), "concluded");
          shm[slot.slot].command = 0;
          pthread_exit(NULL);
          break;
        }
        case EMERGENCY: {
          printf("%s %s EMERGENCY LANDING REQUESTED\n", printTime(), f.flight.flight_code);
          writeFlightToLog('E', &f, checkForClearRunway('L'), "emergency");
          printf("%s ARRIVAL [%s] @ %s started\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          writeFlightToLog('L', &f, checkForClearRunway('L'), "started");
          shm[slot.slot].runway->is_occupied = 1;
          sleep(configs.L * TIME_UNITS);
          shm[slot.slot].runway->is_occupied = 0;
          printf("%s ARRIVAL [%s] @ %s concluded\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          writeFlightToLog('L', &f, checkForClearRunway('L'), "concluded");
          shm[slot.slot].command = 0;
          pthread_exit(NULL);
          break;
        }
        case REDIRECT: {
          printf("%s %s LEAVING TO ANOTHER AIRPORT => FUEL = 0\n", printTime(), f.flight.flight_code);
          shm[slot.slot].command = 0;
          pthread_exit(NULL);
          break;
        }
        case HOLDING: {
          printf("%s %s HOLDING, NEW ETA : %d\n", printTime(), f.flight.flight_code, f.flight.eta + configs.L);
          break;
        }
      }
    }
    usleep(configs.ut * 1000);
  }
  pthread_exit(NULL);
}
void* departureFlight(void *flight) {
  /*
  Sends its flight info to CT
  and receives its designated memory slot.
  Updates stats and executes the command
  written to memory by the CT.
  */
  DepartingListNode f = *((DepartingListNode *) flight);
  //writeFlightToLog('D', &f, checkForClearRunway('D'));

  //update stats
  stats.total_flights_created++;
  stats.n_departures++;

  DepartingFlightMsg dep_msg;
  MemSlotMsg slot;
  long mtype = DEPARTURE_TYPE; //DEPARTURE
  int takeoff_time = f.flight.takeoff_time;
  int init = f.flight.init;

  //Create message
  dep_msg.mtype = mtype;
  dep_msg.takeoff_time = takeoff_time;
  dep_msg.init = init;

  //Add to msq
  if(msgsnd(msq, &dep_msg, sizeof(DepartingFlightMsg) - sizeof(long), 0) < 0) {
    perror("msgsnd error: ");
    exit(1);
  }

  sem_post(received_departure);
  sem_post(mutex_msq);

  //receive shm slot where the thread and CT will communicate
  if(msgrcv(msq, &slot, sizeof(MemSlotMsg) - sizeof(long), DEPARTURE_TYPE, 0) < 0) {
    perror("msgrcv: ");
    exit(1);
  }

  while(1) {
    sem_wait(mutex_shm);
    if(sem_trywait(status_update_departures) == 0) {
      switch(shm[slot.slot].command) {
        case DEPARTURE: {
          printf("%s DEPARTURE [%s] @ %s started\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          writeFlightToLog('D', &f, checkForClearRunway('D'), "started");
          shm[slot.slot].runway->is_occupied = 1;
          sleep(configs.T * TIME_UNITS);
          shm[slot.slot].runway->is_occupied = 0;
          printf("%s DEPARTURE [%s] @ %s concluded\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          writeFlightToLog('D', &f, checkForClearRunway('D'), "concluded");
          shm[slot.slot].command = 0;
          break;
        }
        case WAIT: {
          printf("%s DEPARTURE [%s] @ %s waiting\n", printTime(), f.flight.flight_code, shm[slot.slot].runway->id);
          break;
        }
      }
    }
  }
  pthread_exit(NULL);
}
void* createLandingFlight() {
  pthread_t flight;
  while(1) {
    while(landing_list_head != NULL) {
      if(((time(NULL) - init_time) / TIME_UNITS >= landing_list_head->flight.init)) {
        LandingListNode temp = *landing_list_head; //temp é o valor apontado pela head neste momento
        if(pthread_create(&flight, NULL, landingFlight, &temp) != 0) {
          perror("Couldn't create ARRIVAL flight.");
          exit(1);
        }
        LandingListNode * old_head = landing_list_head;
        landing_list_head = landing_list_head->next;
        free(old_head);
      }
      else break;
    }
    usleep(configs.ut * 1000);
    //printList("ARRIVAL");
  }
  pthread_exit(NULL);
}
void* createDepartingFlight() {
  pthread_t flight;
  while(1) {
    while(departure_list_head != NULL) {
      if(((time(NULL) - init_time) / TIME_UNITS >= departure_list_head->flight.init)) {
        DepartingListNode temp = *departure_list_head; //temp é o valor apontado pela head neste momento
        if(pthread_create(&flight, NULL, departureFlight, &temp) != 0) {
          perror("Couldn't create ARRIVAL flight.");
          exit(1);
        }
        DepartingListNode * old_head = departure_list_head;
        departure_list_head = departure_list_head->next;
        free(old_head);
      }
      else break;
    }
    usleep(configs.ut * 1000);
    //printList("DEPARTURE");
  }
  pthread_exit(NULL);
}
void* LandingScheduler() {
  /*
  Syncs all landing flight commands.
  */
  ReadyToLand *i;
  Runway *runway;
  while(1) {
    sem_wait(timer);
    //sem_wait(mutex_shm);
    for(i = ready_to_land_list_head; i != NULL; i = i->next) {
      if(i->initial_fuel - i->fuel == i->eta) {
        runway = checkForClearRunway('L');
        if(runway->is_occupied != -1) {
          runway->time_left_until_free = configs.L + configs.dl;
          shm[i->slot].command = LANDING;
          shm[i->slot].runway = runway;
          //free(i);
          shm[i->slot].command = 0;
        }
        else {
          //HOLDING
          i->eta += rand() % ((configs.max + 1) - configs.min) + configs.min;
          shm[i->slot].command = HOLDING;
        }
        sem_post(mutex_shm);
        sem_post(status_update_arrivals);
      }
      else if(i->fuel == 4 + i->eta + configs.L) {
        shm[i->slot].command = EMERGENCY;
        runway = checkForClearRunway('L');
        if(runway->is_occupied != -1) {
          shm[i->slot].command = LANDING;
          shm[i->slot].runway = runway;
        }
        else {
          //HOLDING
          shm[i->slot].command = HOLDING;
        }
        //update queue -> put in front
        //sortedInsertReadyArrivals(&ready_to_land_list_head, i);
        //check for clear runway
        // if(checkForClearRunway('L') != -1) {
        //   shm[i->slot].command = REQUESTING_PERMISSION_FOR_LANDING;
        // }
        sem_post(mutex_shm);
        sem_post(status_update_arrivals);
      }
      else if(i->fuel == 0) {
        shm[i->slot].command = REDIRECT;
        sem_post(status_update_arrivals);
        sem_post(mutex_shm);
      }
      i->fuel--;
    }
  }
  pthread_exit(NULL);
}
void* DepartureScheduler() {
  /*
  Syncs all departing flight commands.
  */
  ReadyToFly *i;
  Runway *runway;
  while(1) {
    sem_wait(timer);
    for(i = ready_to_fly_list_head; i != NULL; i = i->next) {
      if(i->takeoff_time == 0) {
        runway = checkForClearRunway('D');
        if(runway->is_occupied != -1) {
          runway->time_left_until_free = configs.T + configs.dt;
          shm[i->slot].command = DEPARTURE;
          shm[i->slot].runway = runway;
          //free(i);
        }
        else {
          i->takeoff_time += runway->time_left_until_free;
          shm[i->slot].command = WAIT;
        }
        sem_post(mutex_shm);
        sem_post(status_update_departures);
      }
      i->takeoff_time--;
    }
  }
  pthread_exit(NULL);
}
MemSlotMsg getSlotIndex(char c) {
  /*
  Searches for a free memory slot.
  Returns the slot upon success,
  and slot = -1 on fail.
  */
  MemSlotMsg slot;
  slot.slot = -1;
  slot.mtype = -1;
  int i;
  for(i = 0; i < SHM_SIZE; i++) {
    if(shm[i].command == FREE) {
      slot.slot = i;
      shm[i].command = OCCUPIED;
      if(c == 'D') {
        slot.mtype = DEPARTURE_TYPE;
      }
      else {
        slot.mtype = ARRIVAL_TYPE;
      }
      break;
    }
  }
  return slot;
}
void initSemaphores() {
  /*
  Initializes all semaphores used.
  */
  sem_unlink(MUTEX_SHM);
  mutex_shm = sem_open(MUTEX_SHM, O_CREAT | O_EXCL, 0700, 1);

  sem_unlink(MUTEX_MSQ);
  mutex_msq = sem_open(MUTEX_MSQ, O_CREAT | O_EXCL, 0700, 0);

  sem_unlink(RECEIVED_LANDING);
  received_landing = sem_open(RECEIVED_LANDING, O_CREAT | O_EXCL, 0700, 0);

  sem_unlink(RECEIVED_DEPARTURE);
  received_departure = sem_open(RECEIVED_DEPARTURE, O_CREAT | O_EXCL, 0700, 0);

  sem_unlink(LANDING_LIST_MUTEX);
  landing_flight_list_mutex = sem_open(LANDING_LIST_MUTEX, O_CREAT | O_EXCL, 0700, 1);

  sem_unlink(DEPARTURE_LIST_MUTEX);
  departing_flight_list_mutex = sem_open(DEPARTURE_LIST_MUTEX, O_CREAT | O_EXCL, 0700, 1);

  sem_unlink(STATUS_UPDATE_ARRIVALS);
  status_update_arrivals = sem_open(STATUS_UPDATE_ARRIVALS, O_CREAT | O_EXCL, 0700, 0);

  sem_unlink(STATUS_UPDATE_DEPARTURES);
  status_update_departures = sem_open(STATUS_UPDATE_DEPARTURES, O_CREAT | O_EXCL, 0700, 0);

  sem_unlink(TIMER);
  timer = sem_open(TIMER, O_CREAT | O_EXCL, 0700, 0);

}
InitialConfig readConfigFile(InitialConfig configs) {
  FILE *f1;
  if ((f1 = fopen(CONFIG_FILE, "r")) == NULL) {
      perror("fopen error.");
      exit(1);
  }
  fscanf(f1, "%d\n%d, %d\n%d, %d\n%d, %d\n%d\n%d", &configs.ut, &configs.T, &configs.dt, &configs.L, &configs.dl, &configs.min, &configs.max, &configs.D, &configs.A);
  return configs;
}
void writeCommandToLogFile(char valid[], char *command) {
  FILE *f = fopen(LOG, "a");
  if (f == NULL) {
    perror("Couldn't open 'log.txt'.");
    exit(1);
  }
  fprintf(f, "%s %s COMMAND => %s\n", printTime(), valid, command);
  fclose(f);
}
int handleLanding(char **info_vector, int num_tokens) {
  /*
  Checks if command for landing is valid.
  Returns 0 if valid, 1 if not.
  */
  int wrong = 0;
  int i, j, k, x;
  //all flights codes must have 2 capital letters
  if((!isupper(info_vector[1][0])) && (!isupper(info_vector[1][1]))) {
    wrong = 1;
  }
  //and at least 3 numbers
  for(i = 2; i < strlen(info_vector[1]); i++) {
    if(!isdigit(info_vector[1][i])) {
      wrong = 1;
      break;
    }
  }
  //'init:' and 'takeoff:' check
  if((strcmp(info_vector[2], "init:") != 0) || (strcmp(info_vector[4], "eta:") != 0) || (strcmp(info_vector[6], "fuel:") != 0)) {
    wrong = 1;
  }
  //init time can have whatever no of digits, but it must be greater than the clock value
  for(j = 0; j < strlen(info_vector[3]); j++) {
    if(!isdigit(info_vector[3][j]) || (atoi(info_vector[3]) < (time(NULL) - init_time))) {
      wrong = 1;
      break;
    }
  }
  //eta can have whatever no of digits, but it must be greater than init time
  for(k = 0; k < strlen(info_vector[5]); k++) {
    if(!isdigit(info_vector[5][k]) || (atoi(info_vector[5]) <= atoi(info_vector[3]))) {
      wrong = 1;
      break;
    }
  }
  //fuel
  if(atoi(info_vector[7]) < atoi(info_vector[5])) {
    wrong = 1;
  }
  for(x = 0; x < strlen(info_vector[7]); x++) {
    if(!isdigit(info_vector[7][x])) {
      wrong = 1;
      break;
    }
  }

  return wrong;
}
int handleDeparture(char **info_vector, int num_tokens) {
  /*
  Checks if command for departure is valid.
  Returns 0 if valid, 1 if not.
  */
  int wrong = 0;
  int i, j, k;
  //all flights codes must have 2 capital letters
  if((!isupper(info_vector[1][0])) && (!isupper(info_vector[1][1]))) {
    wrong = 1;
  }
  //and at least 3 numbers
  for(i = 2; i < strlen(info_vector[1]); i++) {
    if(!isdigit(info_vector[1][i])) {
      wrong = 1;
      break;
    }
  }
  //'init:' and 'takeoff:' check
  if((strcmp(info_vector[2], "init:") != 0) || (strcmp(info_vector[4], "takeoff:") != 0)) {
    wrong = 1;
  }
  //init time can have whatever no of digits, but it must be greater than the clock value
  for(j = 0; j < strlen(info_vector[3]); j++) {
    if(!isdigit(info_vector[3][j]) || (atoi(info_vector[3]) < (time(NULL) - init_time))) {
      wrong = 1;
      break;
    }
  }
  //takeoff time can have whatever no of digits, but it must be greater than init time
  for(k = 0; k < strlen(info_vector[5]); k++) {
    if(!isdigit(info_vector[5][k]) || (atoi(info_vector[5]) <= atoi(info_vector[3]))) {
      wrong = 1;
      break;
    }
  }

  return wrong;
}
void handleCommand(char command[]) {
  /*
  Splits the command into a vector of strings
  and handles the instruction based on the first
  element of that vector (which will be
  DEPARTURE, ARRIVAL or neither).
  */
  if (strcmp(command, "close") == 0) {
    cleanup();
  }
  /*
  8 is the max no of tokens any given command can have
  If it excedes 8, it will ignore the extra tokens
  */
  int num_tokens = 0;
  char *s = " \r\n\t";
  char *token;
  char *info_vector[8];
  char cpy[BUF_SIZE];
  strcpy(cpy, command);

  //get first token
  token = strtok(command, s);
  //walk through other tokens
  while(token != NULL) {
    info_vector[num_tokens] = token;
    num_tokens ++;
    token = strtok(NULL, s);
  }

  //Departures
  if(strcmp(info_vector[0], "DEPARTURE") == 0) {
    if ((handleDeparture(info_vector, num_tokens) == 0 ) && (doesFlightAlreadyExist(info_vector) == 0)) {
      printf("%s NEW COMMAND => %s\n", printTime(), cpy);
      writeCommandToLogFile("NEW", cpy);
      //creates new node, sorts & adds to waiting list
      DepartingListNode *new_flight = newDepartingListNode(info_vector);
      sortedInsertDepartures(&departure_list_head, new_flight);
    }
    else {
      printf("%s WRONG COMMAND => %s\n", printTime(), cpy);
      writeCommandToLogFile("WRONG", cpy);
    }
  }
  //Landings
  else if((strcmp(info_vector[0], "ARRIVAL") == 0)) {
    if((handleLanding(info_vector, num_tokens) == 0) && (doesFlightAlreadyExist(info_vector) == 0)) {
      printf("%s NEW COMMAND => %s\n", printTime(), cpy);
      writeCommandToLogFile("NEW", cpy);
      //creates new node, sorts & adds to waiting list
      LandingListNode *new_flight = newLandingListNode(info_vector);
      sortedInsertArrivals(&landing_list_head, new_flight);
    }
    else {
      printf("%s WRONG COMMAND => %s\n", printTime(), cpy);
      writeCommandToLogFile("WRONG", cpy);
    }
  }
  //!Departures && !Landings
  else /*if ((strcmp(info_vector[0], "DEPARTURE") != 0) && (strcmp(info_vector[0], "ARRIVAL") != 0))*/ {
    printf("%s WRONG COMMAND => %s\n", printTime(), cpy);
    writeCommandToLogFile("WRONG", cpy);
  }
}
void createInputPipe() {
  // Create pipe
  if ((mkfifo(PIPE_NAME, O_CREAT|O_EXCL|0600)<0) && (errno!= EEXIST)) {
    perror("Cannot create pipe: ");
    exit(0);
  } else {
    printf("Input pipe created!\n\n");
  }
}
void createMessageQueue() {
  if((msq = msgget(IPC_PRIVATE, IPC_CREAT | 0700)) < 0) {
    perror("[msq] msgget error.");
    exit(1);
  } else {
    printf("Created [msq]!\n");
  }
}
void createSharedMemory() {
  int i;
  //Create shmem
  if((shmid = shmget(IPC_PRIVATE, sizeof(Command) * SHM_SIZE, IPC_CREAT | 0777)) < 0) {
    perror("[shmid] shmget error.");
    exit(1);
  } else {
    printf("Created [shm]!\n");
  }
  //Attach shmem
  if((shm = (Command *)shmat(shmid, NULL, 0)) < 0) {
    perror("[shmid] shmat error.");
    exit(1);
  } else {
    printf("Attached [shm] to address #%p\n", shm);
  }
  //initialize all memory to 0
  for(i = 0; i < SHM_SIZE; i++) {
    shm[i].command = 0;
  }
}
void initialize() {
  //read config file
  configs = readConfigFile(configs);

  //iniciate shm
  createSharedMemory();

  //iniciate msq
  createMessageQueue();

  //iniciate semaphores
  initSemaphores();

  //initialize time to 0
  init_time = time(NULL);

  //start counting
  pthread_create(&time_counter, NULL, (void*) countTime, NULL);

  //initiate flight-generating threads
  pthread_create(&departures_list_thread, NULL, (void*) createDepartingFlight, NULL);
  pthread_create(&landings_list_thread, NULL, (void*) createLandingFlight, NULL);

  //create named pipe ("input pipe")
  createInputPipe();

  //stats = 0
  stats.total_flights_created = 0;
  stats.n_landings = 0;
  stats.n_departures = 0;
  stats.awt_departure = 0.0;
  stats.awt_landing = 0.0;
  stats.avg_holdings_per_landing = 0.0;
  stats.avg_holdings_per_emergency_landing = 0.0;
  stats.n_diverted_flights = 0;
  stats.n_rejected_flights = 0;

  //runways
  strcpy(l1.id, "L1");
  l1.is_occupied = 0;
  l1.time_left_until_free = 0;
  strcpy(l2.id, "L2");
  l2.is_occupied = 0;
  l2.time_left_until_free = 0;
  strcpy(d1.id, "D1");
  d1.is_occupied = 0;
  d1.time_left_until_free = 0;
  strcpy(d2.id, "D2");
  d2.is_occupied = 0;
  d2.time_left_until_free = 0;
}
void controlTower() {
  /*
  Manages runways and
  controls landings/departures.
  Keeps LL of thread info to
  know when a flight will
  leave/arrive.
  */
  printf("[%d] Im the Control Tower.\n", getpid());

  //receives flight info through MSQ w/ thread
  DepartingFlightMsg dep_msg;
  LandingFlightMsg arr_msg;
  MemSlotMsg dep_slot;
  MemSlotMsg arr_slot;
  ReadyToFly *new_dep;
  ReadyToLand *new_arr;
  pthread_t landing_scheduler;
  pthread_t departure_scheduler;

  while(1) {
    sem_wait(mutex_msq);
    if(sem_trywait(received_departure) == 0) {
      if(msgrcv(msq, &dep_msg, sizeof(DepartingFlightMsg) - sizeof(long), DEPARTURE_TYPE, 0) < 0) {
        perror("msgrcv error: ");
        exit(1);
      }
      dep_slot = getSlotIndex('D');
      new_dep = newReadyToFlyNode(dep_msg, dep_slot);
      sortedInsertReadyDepartures(&ready_to_fly_list_head, new_dep);
      //printReadyList('D');
      pthread_create(&departure_scheduler, NULL, DepartureScheduler, NULL);
      if(msgsnd(msq, &dep_slot, sizeof(MemSlotMsg) - sizeof(long), 0) < 0) {
        perror("msgsnd: ");
        exit(1);
      }
    }
    else if(sem_trywait(received_landing) == 0) {
      if(msgrcv(msq, &arr_msg, sizeof(LandingFlightMsg) - sizeof(long), ARRIVAL_TYPE, 0) < 0) {
        perror("msgrcv error: ");
        exit(1);
      }
      arr_slot = getSlotIndex('A');
      new_arr = newReadyToLandNode(arr_msg, arr_slot);
      sortedInsertReadyArrivals(&ready_to_land_list_head, new_arr);
      //printReadyList('A');
      pthread_create(&landing_scheduler, NULL, LandingScheduler, NULL);
      if(msgsnd(msq, &arr_slot, sizeof(MemSlotMsg) - sizeof(long), 0) < 0) {
        perror("msgsnd: ");
        exit(1);
      }
    }
  }
  pthread_exit(NULL);
}
void simManager() {
  sm_pid = getpid();
  pid_t pid;
  signal(SIGINT, cleanup);
  initialize();
  if((pid = fork()) == 0) {
    //CT is the child process of SimManager
    controlTower();
  }

  printf("-------------------\n\n");
  printf("Reading flights from terminal...\n\n");

  // Opens the pipe for reading
  if ((input_pipe_fd = open(PIPE_NAME, O_RDONLY)) < 0) {
    perror("Cannot open pipe for reading: ");
    exit(0);
  }

  // Read command
  char str[BUF_SIZE];
  int nread = 0;
  while(1) {
    if((nread = read(input_pipe_fd, str, BUF_SIZE - 1)) != 0) {
      str[nread - 1] = '\0';
      handleCommand(str);
    }
    usleep(1);
  }
}
int main() {
  simManager();
  return 0;
}
