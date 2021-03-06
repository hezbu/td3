//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++ Includes ++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "../inc/tcp_socket.h"
#include "file_functions.c"
#include "http_server.c"
#include "query_accelerometer.c"
#include "update_config.c"

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//++++++++++++++++++++++++++ Variables globales +++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
sem_t *update_semaphore, *config_semaphore, *signal_update;
struct config_parameters_st *config_parameters;
int socket_http;
void *shared_mem_ptr = (void *) 0;

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++ Main del TCP Socket +++++++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
int main(int argc, char *argv[]) {

  int client_port, connection, listening, max_conn_i, max_conn_aux, current_conn_aux, shared_mem_id;
  struct sockaddr_in server_address, client_socket_address;
  char client_addr[20];
  socklen_t client_address_lenght;
  pid_t server_pid, http_child, get_val_child, update_conf_child;

  server_pid = getpid();
  printf("[LOG] TCP SOCKET: Server running with pid: %d\n", server_pid);

  // Creamos el socket
  socket_http = socket(AF_INET, SOCK_STREAM,0);   // AF_INET: IPv4 // SOCK_STREAM: TCP // 0: PROTOCOLO NULO
  if (socket_http < 0) {
    perror("[ERROR] TCP SOCKET: Can't create socket");
    return(-1);
  }

  // Asigna el puerto indicado y una IP de la maquina
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(PORT);      // htons/htonl (Host TO Network Short/Long) es para pasar de little endian a big endian
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  memset(server_address.sin_zero, '\0', sizeof server_address.sin_zero);  // Relleno el buffer sin_zero con null

  setsockopt(socket_http, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int));

  // Conecta el socket a la direccion local
  if (bind(socket_http, (struct sockaddr *)&server_address, sizeof(server_address))<0) {
    perror("[ERROR] TCP SOCKET: Can't bind socket");
    close(socket_http);
    return(-1);
  }

  // Configuro la memoria compartida
  shared_mem_id = shmget( (key_t)1234, MEM_SZ, 0666 | IPC_CREAT);
  if (shared_mem_id == -1) {
    perror("[ERROR] TCP SOCKET: shmget failed");
    close(socket_http);
    return(-1);
  }

  shared_mem_ptr = shmat(shared_mem_id, (void *)0, 0);
  if (shared_mem_ptr == (void *)-1) {
    perror("[ERROR] TCP SOCKET: shmat failed");
    close(socket_http);
    return(-1);
  }
  printf("[LOG] TCP SOCKET: Shared memory attached at %p\n", shared_mem_ptr);

  // Inicializo variables
  config_parameters = (struct config_parameters_st *)shared_mem_ptr;
  config_parameters->backlog = 2;
  config_parameters->current_conn=0;
  config_parameters->max_conn=1000;
  config_parameters->prom=5;
  config_parameters->query_freq=1.0;

  // Inicializo el semáforo para poder hacer update de configuracion
  sem_unlink ("config_semaphore");
  config_semaphore = sem_open ("config_semaphore", O_CREAT | O_EXCL, 0644, 1);
  if (config_semaphore < 0){
    perror("[ERROR] TCP SOCKET: Can't create semaphore");
    close(socket_http);
    sem_unlink ("config_semaphore");
    sem_close(config_semaphore);
    shmdt(shared_mem_ptr);
    return(-1);
  }

  // Inicializo el semáforo para poder hacer update del archivo html
  sem_unlink ("update_semaphore");
  update_semaphore = sem_open ("update_semaphore", O_CREAT | O_EXCL, 0644, 1);
  if (update_semaphore < 0){
    perror("[ERROR] TCP SOCKET: Can't create semaphore");
    close(socket_http);
    sem_unlink ("config_semaphore");
    sem_close(config_semaphore);
    sem_unlink ("update_semaphore");
    sem_close(update_semaphore);
    shmdt(shared_mem_ptr);
    return(-1);
  }

  // Inicializo el semáforo para poder hacer update del archivo html
  sem_unlink ("signal_update");
  signal_update = sem_open ("signal_update", O_CREAT | O_EXCL, 0644, 1);
  if (signal_update < 0){
    perror("[ERROR] TCP SOCKET: Can't create semaphore");
    close(socket_http);
    sem_unlink ("signal_update");
    sem_close(signal_update);
    sem_unlink ("config_semaphore");
    sem_close(config_semaphore);
    sem_unlink ("update_semaphore");
    sem_close(update_semaphore);
    shmdt(shared_mem_ptr);
    return(-1);
  }

  // Asigno la señal SIGCHILD al handler del socket
  signal(SIGCHLD, handler_socket_SIGCHLD);

  // Forkeo para poder actualizar los valores
  update_conf_child = fork();

  if (!update_conf_child){  // El proceso hijo va a actualizar los valores cada tanto
    printf("[LOG] TCP SOCKET: Update configuration demon started\n");
    update_configuration();
    exit (0);
  }

  // Forkeo para poder obtener los valores
  get_val_child = fork();

  if (!get_val_child){  // El proceso hijo va a actualizar los valores cada tanto
    printf("[LOG] TCP SOCKET: Update position demon started\n");
    update_http_file();
    exit (0);
  }

  // Asigno la señal SIGUSR1 al handler del socket
  signal(SIGUSR1, handler_socket_SIGUSR1);

  sem_wait (config_semaphore);
  // Indicar que el socket encole hasta backlog pedidos de conexion simultaneas.
  listening = listen(socket_http, config_parameters->backlog);
  sem_post (config_semaphore);
  if (listening < 0) {    // MAX_CONN ES DE LAS QUE ESTAN EN COLA A SER ATENDIDAS, NO CUENTA LAS QUE TENGO ACTIVAS
    perror("[ERROR] TCP SOCKET: Catn't listen");
    close(socket_http);
    sem_unlink ("signal_update");
    sem_close(signal_update);
    sem_unlink ("config_semaphore");
    sem_close(config_semaphore);
    sem_unlink ("update_semaphore");
    sem_close(update_semaphore);
    shmdt(shared_mem_ptr);
    return(-1);
  }

  printf("[LOG] TCP SOCKET: Server is online listening on port: %d\n", PORT);

  // Asigno la señal SIGINT al handler del socket
  signal(SIGINT, handler_socket_SIGINT);

  // Permite atender a multiples usuarios
  while (1) {

    printf("[LOG] TCP SOCKET: Waiting for new connection\n");

    max_conn_i = 1;
    while (max_conn_i){
      sem_wait (config_semaphore);
      max_conn_aux = config_parameters->max_conn;
      current_conn_aux = config_parameters->current_conn;
      sem_post (config_semaphore);
      if (current_conn_aux < max_conn_aux )
        max_conn_i = 0;
    }


    client_address_lenght = sizeof(client_socket_address);
    // La funcion accept rellena la estructura client_socket_address con informacion del cliente y pone en client_address_lenght la longitud de la estructura.
    if ((connection = accept (socket_http, (struct sockaddr *) &client_socket_address, (socklen_t *)&client_address_lenght)) < 0) {  // LE PASO COMO PARAMETRO EL SOCKET DE ESPERA Y ME DEVUELVE EL SOCKET DE LA CONEXION
      perror("[ERROR] TCP SOCKET: Error accepting new connection");                                      // SI LE PASO EL ADRESS ME DEVUELVE LA DIRECCION IP DEL CLIENTE
      close(connection);
    }

    http_child = fork();

    if (http_child < 0) {
      perror("[ERROR] TCP SOCKET: Error forking");
    } else if (http_child == 0){ // Pregunto si es el hijo

      sem_wait (config_semaphore);
      config_parameters->current_conn ++;
      sem_post (config_semaphore);

  		close(socket_http);   // Cierro el socket desde el hijo

      strcpy(client_addr, inet_ntoa(client_socket_address.sin_addr));  // inet_ntoa ME PASA DE IP A ASCII PARA PODER MOSTRARLO
      client_port = ntohs(client_socket_address.sin_port);  // ntohs (Network TO Host Short): Para volver a cambiar el endian
      printf("[LOG] TCP SOCKET: New client connected: %s:%d\n", client_addr, client_port);

      http_server(connection);
      // Cierra la conexion con el cliente actual
      close(connection);

      printf("[LOG] TCP SOCKET: Client %s:%d disconnected\n", client_addr, client_port);

      sem_wait (config_semaphore);
      if (config_parameters->current_conn > 0)
        config_parameters->current_conn --;
      sem_post (config_semaphore);

      // Termino el proceso hijo
      exit (0);
    }

  }
  // Cierra el servidor
  close(socket_http);
  sem_unlink ("signal_update");
  sem_close(signal_update);
  sem_unlink ("config_semaphore");
  sem_close(config_semaphore);
  sem_unlink ("update_semaphore");
  sem_close(update_semaphore);
  shmdt(shared_mem_ptr);

  return 0;
}


//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//++++++++++++++++++++++ Handler de SIGCHILD (socket) +++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void handler_socket_SIGCHLD (int signbr) {
  pid_t pid_child;
  int status_child;
  while((pid_child = waitpid(-1, &status_child, WNOHANG)) > 0) {
    //printf("[LOG] TCP SOCKET: PID of dead child: %d\n", pid_child);
  }
  return;
}

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//+++++++++++++++++++++++ Handler de SIGINT (socket) ++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void handler_socket_SIGINT (int signbr) {
  close(socket_http);
  sem_unlink ("signal_update");
  sem_close(signal_update);
  sem_unlink ("config_semaphore");
  sem_close(config_semaphore);
  sem_unlink ("update_semaphore");
  sem_close(update_semaphore);
  shmdt(shared_mem_ptr);
  printf("[LOG] TCP SOCKET: Exiting\n");
  exit(0);
}

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//++++++++++++++++++++++ Handler de SIGUSR1 (socket) ++++++++++++++++++++++++++
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void handler_socket_SIGUSR1 (int signbr) {
  sem_trywait(signal_update);
  sem_post(signal_update);
  return;
}
