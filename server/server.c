#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "gameLogic.c"
#include "parser.c"

#define MAX_GAMES 1024
#define QUEUE_SIZE 10

//tablica rozgrywanych gier
struct game *games[MAX_GAMES];

//struktura przekazywana do wątku gameManager
typedef struct
{
    game* current_game;
    char player; //którego gracza nasłuchuje gameManager
} game_manager_data;

void initializeGame(game *new_game, int player1_sd) {
    new_game->player1_socket = player1_sd;
    new_game->move_count = 0;
    new_game->last_attack = 60;
    new_game->player_move = 0;
    new_game->is_terminated = 0;
    new_game->mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    for (int i = 0; i < 32; i++) {
        if (i < 12) {
            new_game->board[i] = 1; //pionki gracza 1 w górnej części
            new_game->board_history_p1[i] = 1;
            new_game->board_history_p2[i] = 0;
        } else if (i >= 20) {
            new_game->board[i] = 2; //gracza 2 w dolnej
            new_game->board_history_p1[i] = 0;
            new_game->board_history_p2[i] = 1;
        } else {
            new_game->board[i] = 0; //pozostałe pola puste
            new_game->board_history_p1[i] = 0;
            new_game->board_history_p2[i] = 0;
        }
    }    
}

int findAvailiableGame() { //znajduje wolny slot na grę w games
    for (int i = 0; i < MAX_GAMES; i++) {
        if (games[i] == NULL) {
            return i;
        }
    }
    return -1; //nie znaleziono
}

char sendToClient(int socket, char *buf, int n) { //wysyła do klienta
    int i = 0;
    int s;
    while (n > 0) { //zabezpiecza przed pofragmentowaną wysyłką
        s = send(socket, buf+i, n, 0);
        if (s < 0) {
            return 0;
        }
        i += s;
        n -= s;
    }
    return 1;
}

void playerQuit(game *g, char player) {
    
}

void gameManager(void *t_data) {
    pthread_detach(pthread_self());
    game_manager_data *th_data = (game_manager_data*)t_data;
    game *current_game = (*th_data).current_game;
    char player = (*th_data).player;
    int v;
    char buf[1024];
    int player_socket = 0;
    int other_player_socket = 0;
    if (player == 1) {
        player_socket = current_game->player1_socket;
        other_player_socket = current_game->player2_socket;
    } else {
        player_socket = current_game->player2_socket;
        other_player_socket = current_game->player1_socket;
    }

    while(1) {
        int i;
        v = read(player_socket, buf, 1024);
        pthread_mutex_lock(current_game->mutex); //mutex gry założony
        //na czas przetwarzania odebranego zapytania
        if (v>0) {
            char cmd;
            cmd = parseCommandName(buf);
            if (cmd == 1) {
                char pos1, pos2;
                parseMove(buf, &pos1, &pos2);
                printf("Move %d, %d, player %d\n", pos1, pos2, player);
                i = serializeGame(current_game, buf);
                sendToClient(other_player_socket, buf, i);
                sendToClient(player_socket, buf, i);
            } else if (cmd == 2) {
                playerQuit(current_game, player);
                printf("Player %d quit game\n", player);
                
            }
        } else if (v == 0) { //klient się rozłączył

        }
        pthread_mutex_unlock(current_game->mutex);
    }


    pthread_exit(NULL);
}

void connectionListener(int server_socket_descriptor) {
    int connection_socket_descriptor;
    int awaiting_game = -1; //gra, która czeka na dołączenie graczy
    int awaiting_player_socket_descriptor;
    int create_result1 = 0;
    int create_result2 = 0;

    while(1)
    {
        connection_socket_descriptor = accept(server_socket_descriptor, NULL, NULL);
        if (connection_socket_descriptor < 0)
        {
            fprintf(stderr, "Błąd przy próbie utworzenia gniazda dla połączenia.\n");
            exit(1);
        }
        printf("Połączenie...\n");

        if (awaiting_game == -1) { //żadna gra nie oczekuje, trzeba otworzyć nową
            printf("Tworzenie nowej poczekalni...\n");
            awaiting_game = findAvailiableGame();
            if (awaiting_game == -1) { //wszystkie zajęte
                close(connection_socket_descriptor); //serwer zapełniony, orzuca połączenie
            } else { //mamy gracza 1, każemy mu czekać
                awaiting_player_socket_descriptor = connection_socket_descriptor;
                game *new_game = malloc(sizeof(game));
                initializeGame(new_game, awaiting_player_socket_descriptor);
                games[awaiting_game] = new_game;
                char buf[128] = "await;\n";
                send(awaiting_player_socket_descriptor, buf, strlen(buf), 0);
            }
        } else { //jakaś gra oczekuje
            printf("Poczekalnia pełna, tworzenie gry...\n");
            game *aw_game = games[awaiting_game];
            aw_game -> player2_socket = connection_socket_descriptor;
            awaiting_game = -1;
            //obaj gracze się połączyli, odpalamy gameManager dla każdego z nich
            game_manager_data *data_p1 = malloc(sizeof(game_manager_data));
            game_manager_data *data_p2 = malloc(sizeof(game_manager_data));
            data_p1 -> current_game = aw_game;
            data_p1 -> player = 1;
            data_p2 -> current_game = aw_game;
            data_p2 -> player = 2;
            pthread_t thread1;
            pthread_t thread2;
            char buf[128] = "start;\n";
            send(awaiting_player_socket_descriptor, buf, strlen(buf), 0);
            send(connection_socket_descriptor, buf, strlen(buf), 0);
            create_result1 = pthread_create(&thread1, NULL, gameManager, (void *)data_p1);
            if (create_result1){
                printf("Błąd przy próbie utworzenia wątku, kod błędu: %d\n", create_result1);
                exit(-1);
            }
            create_result2 = pthread_create(&thread2, NULL, gameManager, (void *)data_p2);
            if (create_result2){
                printf("Błąd przy próbie utworzenia wątku, kod błędu: %d\n", create_result2);
                exit(-1);
            }
        }
        
        //handleConnection(connection_socket_descriptor);
    }
}

int main(int argc, char *argv[])
{
    int server_socket_descriptor;
    long server_port = 1234;
    int bind_result;
    int listen_result;
    char reuse_addr_val = 1;
    struct sockaddr_in server_address;
    
    if(argc == 2) server_port = strtol(argv[1], NULL, 0);
    //inicjalizacja gniazda serwera

    memset(&server_address, 0, sizeof(struct sockaddr));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(server_port);

    server_socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_descriptor < 0)
    {
        fprintf(stderr, "%s: Błąd przy próbie utworzenia gniazda..\n", argv[0]);
        exit(1);
    }
    setsockopt(server_socket_descriptor, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_addr_val, sizeof(reuse_addr_val));

    bind_result = bind(server_socket_descriptor, (struct sockaddr*)&server_address, sizeof(struct sockaddr));
    if (bind_result < 0)
    {
        fprintf(stderr, "%s: Błąd przy próbie dowiązania adresu IP i numeru portu do gniazda.\n", argv[0]);
        exit(1);
    }

    listen_result = listen(server_socket_descriptor, QUEUE_SIZE);
    if (listen_result < 0) {
        fprintf(stderr, "%s: Błąd przy próbie ustawienia wielkości kolejki.\n", argv[0]);
        exit(1);
    }
    
    printf("Zainicjowano serwer na porcie %d \n", server_port);
    printf("Odbieranie...\n");
    
    connectionListener(server_socket_descriptor);

    close(server_socket_descriptor);
    return(0);
}
