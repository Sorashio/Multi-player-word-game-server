#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 52505
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

void remove_active_player(struct game_state* game, struct client* p);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf);
void broadcast_without_inturn(struct game_state* game, char* outbuf);
void announce_turn(struct game_state *game);
void announce_winner(struct game_state *game, struct client *winner);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);

/* check if player exists */
int check_name(const struct game_state* game, const char* name);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}


int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&(game.head), p->fd);
            };
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                int active_found = 0;
                // Check if this socket descriptor is an active player
                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd != p->fd) {
                        continue;
                    }
                    
                    active_found = 1;
                    size_t bytes_to_read = p->inbuf + MAX_BUF - p->in_ptr;
                    ssize_t bytes_read = read(cur_fd, p->in_ptr, bytes_to_read);
                    if (bytes_read <= 0) {
                        remove_active_player(&game, p);
                        break;
                    }

                    printf("[%d] Reads %ld bytes\r\n", cur_fd, bytes_read);
                    p->in_ptr[bytes_read] = 0;
                    char* lf_pos = strstr(p->in_ptr, "\r\n");
                    if (lf_pos == NULL) {
                        // no cr lf found, read again
                        p->in_ptr += bytes_read;
                        break;
                    }

                    printf("Found a newline %s", p->inbuf);
                    p->in_ptr = p->inbuf;
                    if (game.has_next_turn != p) {
                        char* msg = "It is not your turn to guess\r\n";
                        if (write(p->fd, msg, strlen(msg)) == -1) {
                            remove_active_player(&game, p);
                        }
                        break;
                    }

                    
                    int is_valid = 0;
                    char ch = p->inbuf[0];
                    if (strlen(p->inbuf) == 3 && (ch >= 'a' && ch <= 'z')) {
                        is_valid = 1;
                    }

                    if (is_valid == 0) {
                        char* msg = "You can only guess one letter from a-z! Your Guess?\r\n";
                        if (write(p->fd, msg, strlen(msg)) == -1) {
                         
                            remove_active_player(&game, p);
                        }
                        break;
                    }

                    int i;
                    for (i=0; i<strlen(game.word); ++i) {
                        if (game.word[i] == ch) {
                            break;
                        }
                    }
                    
                    int game_over = 0;
                    int guess_failed = 0;
                    if (i == strlen(game.word)) {
                        // ch is not found in the word
                        char msg_buf[MAX_MSG];
                        sprintf(msg_buf, "%c is not in the word\r\n", ch);
                        if (write(p->fd, msg_buf, strlen(msg_buf)) == -1) {
                          
                            remove_active_player(&game, p);
                        }
                        game.guesses_left--;
                        guess_failed = 1;
                    } else {
                        for (; i<strlen(game.word); ++i) {
                            if (game.word[i] == ch) {
                                game.guess[i] = ch;
                            }
                        }
                    }

                    game.letters_guessed[ch-'a'] = 1;
                    {
                        char msg_buf[MAX_MSG];
                        sprintf(msg_buf, "%s guess %c\r\n", p->name, ch);
                        if (write(p->fd, msg_buf, strlen(msg_buf)) == -1) {
                            
                            remove_active_player(&game, p);
                        };

                        if (strcmp(game.word, game.guess) == 0) {
                            sprintf(msg_buf, "The word was %s.\r\n", game.word);
                            if (write(p->fd, msg_buf, strlen(msg_buf)) == -1) {
                            
                                remove_active_player(&game, p);
                            }

                            sprintf(msg_buf, "Game over! You win!\r\n");
                            if (write(p->fd, msg_buf, strlen(msg_buf)) == -1) {
                              
                                remove_active_player(&game, p);
                            }

                            sprintf(msg_buf, "Game over! %s win!\r\n", p->name);
                            broadcast_without_inturn(&game, msg_buf);
                            game_over = 1;
                        } else if (game.guesses_left == 0) {
                            sprintf(msg_buf, "No guesses left. Game over.\r\n");
                            broadcast(&game, msg_buf);
                            game_over = 1;
                        }

                        if (game_over) {
                            guess_failed = 1;
                            init_game(&game, argv[1]);
                        }
                        status_message(msg_buf, &game);
                        broadcast(&game, msg_buf);
                    }

                    if (guess_failed) {
                        advance_turn(&game);
                    }

                    break;
                }

                

                if (active_found) {
                    announce_turn(&game);
                    if (game.has_next_turn) {
                    char* msg = "You Guess?\r\n";
                    if (write(game.has_next_turn->fd, msg, strlen(msg)) == -1) {
                        
                        remove_active_player(&game, p);
                    }

                    }
                    
                    continue;
                }
        
                // Check if any new players are entering their names
                struct client* prev_player = new_players;
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd != p->fd) {
                        prev_player = p;
                        continue;
                    }

                    // handle input from an new client who has
                    // not entered an acceptable name.
                    size_t bytes_to_read = p->inbuf + MAX_BUF - p->in_ptr;
                    ssize_t bytes_read = read(cur_fd, p->in_ptr, bytes_to_read);
                    if (bytes_read <= 0) {
                        // read error or peer closed socket
                        remove_player(&(new_players), p->fd);
                        break;
                    }
                    
                    printf("[%d] Reads %ld bytes\n", cur_fd, bytes_read);
                    p->in_ptr[bytes_read] = 0;
                    char* lf_pos = strstr(p->in_ptr, "\r\n");
                    if (lf_pos == NULL) {
                        // no cr lf found, read again
                        p->in_ptr += bytes_read;
                        break;
                    }

                    // cr lf found
                    printf("Found a newline %s", p->inbuf);
                    size_t bytes_to_copy = lf_pos - p->inbuf + 1;
                    if (bytes_to_copy <= MAX_NAME) {
                        lf_pos[0] = 0;
                        strcpy(p->name, p->inbuf);
                    } else {
                        // input too many letters
                        strncpy(p->name, p->inbuf, MAX_NAME-1);
                        p->name[MAX_NAME-1] = 0;
                    }

                    if (strlen(p->name) == 0) {
                        // empty string
                        char* msg = "empty name, what's your name: ";
                        if (write(p->fd, msg, strlen(msg)) == -1) {
                            remove_player(&new_players, p->fd);
                            break;
                        }
                        p->in_ptr = p->inbuf;
                        continue;
                    }
                    if (check_name(&game, p->name)) {
                        // player with the same name already exists
                        char msg_buf[256];
                        sprintf(msg_buf, "%s already exists, enter another name: ", p->name);
                        if (write(p->fd, msg_buf, strlen(msg_buf)) == -1) {
                            remove_player(&new_players, p->fd);
                            break;
                        }
                        p->in_ptr = p->inbuf;
                        continue;
                    }
                    
                    if (lf_pos + strlen("\r\n") < p->in_ptr + bytes_read) {
                        printf("discard remaining data\n");
                    }

                    // reset
                    p->in_ptr = p->inbuf;

                    // move from new_players
                    if (prev_player == p) {
                        new_players = p->next;
                    } else {
                        prev_player->next = p->next;
                    }

                    // add to game.head
                    if (game.head == NULL) {
                        p->next = NULL;
                        game.head = p;
                    } else {
                        p->next = game.head;
                        game.head = p;
                    }

                    {
                        // broadcast
                        char msg_buf[256];
                        sprintf(msg_buf, "%s has just joined\n", p->name);
                        broadcast(&game, msg_buf);
                    }

                    if (game.has_next_turn == NULL) {
                        advance_turn(&game);
                    }

                    {
                        char status_msg[MAX_MSG];
                        status_message(status_msg, &game);
                        if (write(p->fd, status_msg, strlen(status_msg)) == -1) {
                            remove_active_player(&game, p);
                        }
                    }
                    

                    if (game.has_next_turn) {
                        // prompt the player whose turn it is with the query "Your guess?"
                        char* msg = "You Guess?\r\n";
                        if (write(game.has_next_turn->fd, msg, strlen(msg)) == -1) {
                            remove_active_player(&game, p);
                        }
                    }

                    {
                        // tell everyone else whose turn it is
                        announce_turn(&game);
                    }

                    break;
                }
            }
        }
    }
    return 0;
}

int check_name(const struct game_state* game, const char* name)
{
    struct client* p = game->head;
    for (; p != NULL; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            return 1;
        }
    }

    return 0;
}

void broadcast(struct game_state *game, char *outbuf)
{
    struct client* p = game->head;
    while (p) {
        struct client* q = NULL;
        if (write(p->fd, outbuf, strlen(outbuf)) == -1) {
            fprintf(stderr, "Write to %s failed\n", p->name);
            q = p->next;
            remove_player(&(game->head), p->fd);
        }

        p = (q != NULL) ? q : p->next;
    }
}

void broadcast_without_inturn(struct game_state* game, char* outbuf)
{
    struct client* p = game->head;
    while (p) {
        struct client* q = NULL;
        if (p != game->has_next_turn) {
            if (write(p->fd, outbuf, strlen(outbuf)) == -1) {
                fprintf(stderr, "Write to %s failed\n", p->name);
                q = p->next;
                remove_player(&(game->head), p->fd);
            }
        }

        p = (q != NULL) ? q : p->next;
    }
}

void announce_turn(struct game_state *game)
{
    char msg_buf[MAX_MSG];
    if (game->has_next_turn == NULL) {
        return;
    }
    sprintf(msg_buf, "It's %s's turn.\r\n", game->has_next_turn->name);
    broadcast_without_inturn(game, msg_buf);
}

void advance_turn(struct game_state *game)
{
    if (game->head == NULL) {
        return;
    }

    if (game->has_next_turn == NULL) {
        game->has_next_turn = game->head;
    } else {
        game->has_next_turn = game->has_next_turn->next;
    }

    if (game->has_next_turn == NULL) {
        game->has_next_turn = game->head;
    }

    return;
}

void remove_active_player(struct game_state* game, struct client* p)
{

    struct client* prev;
    struct client* curr;

    if (game == NULL || p == NULL || game->head == NULL) {
        return;
    }

    prev = NULL;
    curr = game->head;
    while (curr != p) {
        prev = curr;
        curr = curr->next;
    }

    if (prev == NULL) {
        game->head = NULL;
        game->has_next_turn = NULL;
    } else {
        if (game->has_next_turn == p) {
            advance_turn(game);
        }
        prev->next = p->next;
    }

    printf("Removing client %d %s\n", p->fd, inet_ntoa(p->ipaddr));
    FD_CLR(p->fd, &allset);
    close(p->fd);
    free(p);
}




