// Sebastian Diaz G
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>

// ------------------- Utils -------------------
#define MAX_PATH_POINTS 4096

static int abs_i(int x){return x<0?-x:x;}
static int manhattan(int x1,int y1,int x2,int y2){ return abs_i(x1-x2) + abs_i(y1-y2); }

// micro-sleep
static inline void sleep_us(int us) {
    if (us <= 0) return;
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000L;
    nanosleep(&ts, NULL);
}

// ANSI clear screen
static inline void ansi_clear(void){ printf("\033[H\033[J"); }

// --------------------------- Barrier -----------
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int parties;   // numero de hilos participantes
    int count;     // cuenta regresiva para el ciclo actual
    int cycle;     // generacion de la barrera
} barrier_t;

static void barrier_init(barrier_t *b, int parties){
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->cv, NULL);
    b->parties = parties;
    b->count = parties;
    b->cycle = 0;
}

static void barrier_wait(barrier_t *b){
    pthread_mutex_lock(&b->mtx);
    int cycle = b->cycle;
    if (--b->count == 0){
        /* ultimo hilo en llegar: avanzar la generacion y despertar a todos. */
        b->cycle++;
        b->count = b->parties;
        pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->mtx);
    } else {
        while (cycle == b->cycle){
            pthread_cond_wait(&b->cv, &b->mtx);
        }
        pthread_mutex_unlock(&b->mtx);
    }
}

// --------------------------- World Types -------------------------
typedef struct { int x,y; } Point;

typedef struct {
    int width, height; // grid size
} Grid;

typedef struct {
    int x, y;
    int hp;
    int attack;
    int attack_range;
    bool alive;
} Actor;

typedef struct {
    Actor a;
    Point *path;
    int path_len;     // numero de waypoints (excluyendo la posicion inicial)
    int path_idx;     // siguiente indice de waypoint
    bool engaged;     // verdadero si esta peleando (dejar de moverse)
    pthread_t th;
    
} Hero;

typedef struct {
    int id;
    Actor a;
    int vision;
    bool alerted;
    pthread_t th;
} Monster;

// --------------------------- Global State ------------------------
static Grid G;
static Hero *heroes = NULL;
static int H = 0;                         // number of heroes
static Monster *monsters = NULL;
static int M = 0;                         // number of monsters
static volatile int simulation_over = 0;  // end flag

static pthread_mutex_t world_mtx = PTHREAD_MUTEX_INITIALIZER;
static barrier_t tick_barrier;
static barrier_t tick_barrier2;

// Pausa entre ticks para legibilidad (microsegundos)
static int tick_us = 0; // >0 para ralentizar la simulacion

// ASCII visualization flags
static int ascii_live = 0;       // renderear ASCII cada tick (animado)
static int ascii_only = 0;       // renderizar una vez y salir
static int ascii_show_path = 1;  // dibujar el camino planeado como '.'

// --------------------------- Helper Queries ----------------------
static bool any_monster_alive_in_range(int x, int y, int range, int *out_idx){
    int best_i = -1;
    for (int i=0;i<M;i++){
        if (!monsters[i].a.alive) continue;
        if (manhattan(x,y, monsters[i].a.x, monsters[i].a.y) <= range){
            best_i = i; break; // primer coincdcia
        }
    }
    if (out_idx) *out_idx = best_i;
    return best_i != -1;
}

static bool any_monster_alive(){
    for (int i=0;i<M;i++) if (monsters[i].a.alive) return true;
    return false;
}

static void alert_neighbors(int src_idx){
    Monster *src = &monsters[src_idx];
    for (int j=0;j<M;j++){
        if (j==src_idx) continue;
        if (!monsters[j].a.alive) continue;
        int d = manhattan(src->a.x, src->a.y, monsters[j].a.x, monsters[j].a.y);
        if (d <= src->vision) monsters[j].alerted = true;
    }
}

// --------------------------- Actions -----------------------------
static void hero_act_index(int h){
    Hero *hh = &heroes[h];
    if (!hh->a.alive) {
        hh->engaged = false; // Si esta muerto, no esta peleando
        return;
    }

    // Combatir si algun monstruo esta dentro del rango de ataque
    int target = -1;
    if (any_monster_alive_in_range(hh->a.x, hh->a.y, hh->a.attack_range, &target)){
        hh->engaged = true;
        if (target >= 0){
            Monster *m = &monsters[target];
            m->a.hp -= hh->a.attack;
            if (m->a.hp <= 0) { m->a.hp = 0; m->a.alive = false; }
        }
        return; // No moverse mientras esta peleando
    }
    hh->engaged = false;

    // Movimiento sobre el camino
    if (hh->path_idx < hh->path_len){
        Point wp = hh->path[hh->path_idx];
        if (hh->a.x < wp.x) hh->a.x++;
        else if (hh->a.x > wp.x) hh->a.x--;
        else if (hh->a.y < wp.y) hh->a.y++;
        else if (hh->a.y > wp.y) hh->a.y--;
        if (hh->a.x == wp.x && hh->a.y == wp.y) hh->path_idx++;
    }
}


static void monster_act(int i){
    Monster *m = &monsters[i];
    if (!m->a.alive) return;

    // buscar el heroe mas cercano vivo
    int best_h = -1;
    int best_dist = 0;
    for (int h=0; h<H; ++h){
        if (!heroes[h].a.alive) continue;
        int d0 = manhattan(m->a.x, m->a.y, heroes[h].a.x, heroes[h].a.y);
        if (best_h == -1 || d0 < best_dist){ best_h = h; best_dist = d0; }
    }
    if (best_h == -1) return; // no heroes vivos

    int d = best_dist;

    // Ver heroe -> alertar vecinos
    if (!m->alerted && d <= m->vision){
        m->alerted = true;
        alert_neighbors(i);
    }

    // Attack?
    if (d <= m->a.attack_range){
        Hero *t = &heroes[best_h];
    t->a.hp -= m->a.attack;
    if (t->a.hp <= 0) { t->a.hp = 0; t->a.alive = false; }
        return;
    }

    // Moverse hacia el heroe mas cercano si esta alertado
    if (m->alerted){
        Hero *t = &heroes[best_h];
        if (m->a.x < t->a.x) m->a.x++;
        else if (m->a.x > t->a.x) m->a.x--;
        else if (m->a.y < t->a.y) m->a.y++;
        else if (m->a.y > t->a.y) m->a.y--;
    }
}

// --------------------------- Threads -----------------------------
static void *hero_thread(void *arg){
    int h = (int)(intptr_t)arg;
    for(;;){
        pthread_mutex_lock(&world_mtx);
        hero_act_index(h);
        pthread_mutex_unlock(&world_mtx);

        if (tick_us>0) sleep_us(tick_us);
        /* Fase A: finalizar las acciones de este tick. */
        barrier_wait(&tick_barrier);

        /* Fase B: esperar a que el supervisor decida si la simulacion ha terminado.
        El supervisor establecera simulation_over entre las dos barreras. */
        barrier_wait(&tick_barrier2);

        if (simulation_over) break;
    }
    return NULL;
}

static void *monster_thread(void *arg){
    int idx = (int)(intptr_t)arg;
    for(;;){
        pthread_mutex_lock(&world_mtx);
        monster_act(idx);
        pthread_mutex_unlock(&world_mtx);

        if (tick_us>0) sleep_us(tick_us);
    /* Fase A: terminar las acciones para este tick.*/
    barrier_wait(&tick_barrier);

    /* Fase B: esperar la decision del supervisor*/
    barrier_wait(&tick_barrier2);

    if (simulation_over) break;
    }
    return NULL;
}

// --------------------------- Parser ------------------------------
static char *ltrim(char *s){ while(*s && isspace((unsigned char)*s)) s++; return s; }
static void rtrim_inplace(char *s){ size_t n=strlen(s); while(n>0 && isspace((unsigned char)s[n-1])) s[--n]='\0'; }

static int parse_path_points(const char *src, Point **out_path, int *out_len){
    Point *buf = malloc(sizeof(Point)*MAX_PATH_POINTS);
    if(!buf) return -1;
    int count = 0;
    const char *p = src;
    while (*p){
        while (*p && *p!='(') p++;
        if (!*p) break;
        int x,y,adv=0;
        int n = sscanf(p, " ( %d , %d ) %n", &x, &y, &adv);
        if (n == 2){
            if (count < MAX_PATH_POINTS){ buf[count].x=x; buf[count].y=y; count++; }
            p += adv;
        } else break;
    }
    *out_path = buf; *out_len = count;
    return 0;
}

typedef struct { int hp, atk, vis, rng, x, y; bool seen_hp, seen_atk, seen_vis, seen_rng, seen_xy; } MProps;

static int load_config(const char *path){
    FILE *f = fopen(path, "r");
    if(!f){ perror("fopen"); return -1; }

    // Defaults
    G.width=20; G.height=15;
    /* asignar al menos un heroe por defecto */
    if (heroes == NULL){
        H = 1;
        heroes = calloc(H, sizeof(Hero));
    }
    heroes[0].a.hp=100; heroes[0].a.attack=10; heroes[0].a.attack_range=1; heroes[0].a.alive=true;
    heroes[0].path=NULL; heroes[0].path_len=0; heroes[0].path_idx=0; heroes[0].engaged=false; heroes[0].a.x=0; heroes[0].a.y=0;
    M=0; monsters=NULL;

    char line[1024];
    int monster_count_declared = -1;
    MProps *mp = NULL;
    int hero_count_declared = -1;

    while (fgets(line, sizeof(line), f)){
        rtrim_inplace(line);
        char *s = ltrim(line);
        if (*s=='\0' || *s=='#') continue;

        if (strncmp(s, "GRID_SIZE", 9)==0){
            sscanf(s+9, "%d %d", &G.width, &G.height);
        } else if (strncmp(s, "HERO_COUNT", 10)==0){
            sscanf(s+10, "%d", &hero_count_declared);
            if (hero_count_declared < 1 || hero_count_declared > 10000){ fprintf(stderr, "Bad HERO_COUNT\n"); fclose(f); return -1; }
            if (hero_count_declared > 0){
                /* asignar o redimensionar el array (arreglo) de heroes a la cantidad declarada */
                if (heroes == NULL){
                    H = hero_count_declared;
                    heroes = calloc(H, sizeof(Hero));
                } else if (hero_count_declared != H){
                    Hero *tmp = realloc(heroes, sizeof(Hero) * hero_count_declared);
                    if (!tmp){ fprintf(stderr, "OOM allocating heroes\n"); fclose(f); return -1; }
                    /* inicializar nuevas entradas si se expande */
                    if (hero_count_declared > H){
                        for (int hh = H; hh < hero_count_declared; ++hh){
                            tmp[hh].a.hp = 100; tmp[hh].a.attack = 10; tmp[hh].a.attack_range = 1; tmp[hh].a.alive = true;
                            tmp[hh].path = NULL; tmp[hh].path_len = 0; tmp[hh].path_idx = 0; tmp[hh].engaged = false; tmp[hh].a.x = 0; tmp[hh].a.y = 0;
                        }
                    }
                    heroes = tmp;
                    H = hero_count_declared;
                }
            }
        } else if (strncmp(s, "HERO_HP", 7)==0){
            sscanf(s+7, "%d", &heroes[0].a.hp);
        } else if (strncmp(s, "HERO_ATTACK_DAMAGE", 19)==0){
            sscanf(s+19, "%d", &heroes[0].a.attack);
        } else if (strncmp(s, "HERO_ATTACK_RANGE", 18)==0){
            sscanf(s+18, "%d", &heroes[0].a.attack_range);
        } else if (strncmp(s, "HERO_START", 10)==0){
            sscanf(s+10, "%d %d", &heroes[0].a.x, &heroes[0].a.y);
        } else if (strncmp(s, "HERO_PATH", 9)==0){
            parse_path_points(s+9, &heroes[0].path, &heroes[0].path_len);
        } else if (strncmp(s, "HERO_", 5)==0){
            /* Entradas HERO_n_*: asignar al elemento heroes[idx-1] si esta dentro del rango. */
            int idx = -1; char key[64];
            if (sscanf(s, "HERO_%d_%63s", &idx, key) == 2){
                if (idx >= 1 && idx <= H){
                    Hero *hh = &heroes[idx-1];
                    if (strcmp(key, "HP")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1) hh->a.hp = v; }
                    else if (strcmp(key, "ATTACK_DAMAGE")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1) hh->a.attack = v; }
                    else if (strcmp(key, "ATTACK_RANGE")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1) hh->a.attack_range = v; }
                    else if (strcmp(key, "START")==0){ int x,y; if (sscanf(strchr(s,' '), "%d %d", &x,&y)==2){ hh->a.x = x; hh->a.y = y; } }
                    else if (strcmp(key, "PATH")==0){ char *p = strchr(s, '('); if (p) parse_path_points(p, &hh->path, &hh->path_len); }
                } else {
                    /* indice fuera de rango: ignorar */
                }
            }
        } else if (strncmp(s, "MONSTER_COUNT", 13)==0){
            sscanf(s+13, "%d", &monster_count_declared);
            if (monster_count_declared<0 || monster_count_declared>10000){ fprintf(stderr, "Bad MONSTER_COUNT\n"); fclose(f); return -1; }
            M = monster_count_declared;
            monsters = calloc(M, sizeof(Monster));
            mp = calloc(M, sizeof(MProps));
            if(!monsters||!mp){ fprintf(stderr, "OOM allocating monsters\n"); fclose(f); return -1; }
        } else if (strncmp(s, "MONSTER_", 8)==0){
            // MONSTER_i_HP v, MONSTER_i_COORDS x y, etc.
            int idx = -1; char key[64];
            if (sscanf(s, "MONSTER_%d_%63s", &idx, key) == 2){
                if (idx<1 || idx> M){ fprintf(stderr, "Monster index %d out of range\n", idx); fclose(f); return -1; }
                int k = idx-1;
                if (strcmp(key, "HP")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1){ mp[k].hp=v; mp[k].seen_hp=true; } }
                else if (strcmp(key, "ATTACK_DAMAGE")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1){ mp[k].atk=v; mp[k].seen_atk=true; } }
                else if (strcmp(key, "VISION_RANGE")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1){ mp[k].vis=v; mp[k].seen_vis=true; } }
                else if (strcmp(key, "ATTACK_RANGE")==0){ int v; if (sscanf(strchr(s,' '), "%d", &v)==1){ mp[k].rng=v; mp[k].seen_rng=true; } }
                else if (strcmp(key, "COORDS")==0){ int x,y; if (sscanf(strchr(s,' '), "%d %d", &x,&y)==2){ mp[k].x=x; mp[k].y=y; mp[k].seen_xy=true; } }
            }
        }
    }
    fclose(f);

    if (M>0){
        for (int i=0;i<M;i++){
            monsters[i].id = i+1;
            monsters[i].a.hp = mp[i].seen_hp ? mp[i].hp : 50;
            monsters[i].a.attack = mp[i].seen_atk ? mp[i].atk : 10;
            monsters[i].vision = mp[i].seen_vis ? mp[i].vis : 5;
            monsters[i].a.attack_range = mp[i].seen_rng ? mp[i].rng : 1;
            monsters[i].a.x = mp[i].seen_xy ? mp[i].x : 0;
            monsters[i].a.y = mp[i].seen_xy ? mp[i].y : 0;
            monsters[i].a.alive = true;
            monsters[i].alerted = false;
        }
    }
    /* La cantidad de heroes declarada (hero_count_declared) se gestiono mediante la asignacion de memoria al arreglo heroes[] arriba */
    free(mp);

    // --- Validaciones post-parse ---
    if (G.width < 1 || G.height < 1){ fprintf(stderr, "Bad GRID_SIZE\n"); return -1; }
    if (H < 1){ fprintf(stderr, "At least one hero required\n"); return -1; }
#define IN(v,lo,hi) ((v) >= (lo) && (v) <= (hi))
    for (int h=0; h<H; ++h){
        Hero *hh = &heroes[h];
        if (hh->a.hp < 0 || hh->a.attack < 0 || hh->a.attack_range < 0){ fprintf(stderr, "Hero %d has negative params\n", h+1); return -1; }
        if (!IN(hh->a.x, 0, G.width) || !IN(hh->a.y, 0, G.height)){ fprintf(stderr, "Hero %d start OOB\n", h+1); return -1; }
        for (int i=0;i<hh->path_len;i++){
            if (!IN(hh->path[i].x, 0, G.width) || !IN(hh->path[i].y, 0, G.height)){
                fprintf(stderr, "Hero %d path[%d] OOB\n", h+1, i); return -1;
            }
        }
    }
    for (int i=0;i<M;i++){
        Monster *mm = &monsters[i];
        if (mm->a.hp < 0 || mm->a.attack < 0 || mm->a.attack_range < 0 || mm->vision < 0){ fprintf(stderr, "Monster %d has negative params\n", i+1); return -1; }
        if (!IN(mm->a.x, 0, G.width) || !IN(mm->a.y, 0, G.height)){ fprintf(stderr, "Monster %d coords OOB\n", i+1); return -1; }
    }
    /* end validations */

    return 0;
}

// ------------ pretty/ ASCII -------------
static void print_state(int tick){
    printf("Tick %d\n", tick);
    for (int h=0; h < H; ++h){
        Hero *hh = &heroes[h];
        printf(" HERO%02d (%d,%d) HP=%d %s\n",
               h+1, hh->a.x, hh->a.y, hh->a.hp,
               hh->engaged ? "[PELEANDO]" : "");
    }
    for (int i = 0; i < M; i++){
        Monster *m = &monsters[i];
    const char *life = m->a.alive ? "VIVO" : "MUERTO";
    const char *alrt = (m->a.alive && m->alerted) ? "ALERTADO" : "";
    printf("  M%02d (%d,%d) HP=%d %s %s\n",
           m->id, m->a.x, m->a.y, m->a.hp,
           life,
           alrt);
    }
}

static void render_ascii_grid(int tick, const char *title){
    const int W = G.width, GH = G.height;

    // reservar una cuadricula de (GH+1 x W+1) ya que las coordenadas (de 0 a W y 0 a GH) son inclusivas
    char **grid = (char**)malloc((GH+1)*sizeof(char*));
    for (int y=0; y<=GH; ++y){
        grid[y] = (char*)malloc((W+1)*sizeof(char));
        memset(grid[y], ' ', (W+1)*sizeof(char));
    }

    // draw paths for each hero as '.'
    if (ascii_show_path) {
        for (int h = 0; h < H; ++h) {
            Hero *hh = &heroes[h];
            if (!hh->path || hh->path_len <= 0) continue; // Salta si no hay camino

            int px = hh->a.x, py = hh->a.y; // Posicion de partida para el tramo actual

            // Marca el punto de partida (si esta dentro del grid)
            if (py >= 0 && py <= GH && px >= 0 && px <= W) {
                grid[py][px] = '.';
            }

            // Recorre cada punto objetivo (waypoint) del camino
            for (int i = 0; i < hh->path_len; i++) {
                int tx = hh->path[i].x; // Coordenada X objetivo
                int ty = hh->path[i].y; // Coordenada Y objetivo

                // --- Calculo de pasos sx y sy
                int sx = 0; // Paso horizontal por defecto es 0
                if (tx > px) {
                    sx = 1; // Si el objetivo esta a la derecha, paso es +1
                } else if (tx < px) {
                    sx = -1; // Si el objetivo esta a la izquierda, paso es -1
                }   // Si tx == px, sx se queda en 0

                int sy = 0; // Paso vertical por defecto es 0
                if (ty > py) {
                    sy = 1; // Si el objetivo esta arriba, paso es +1
                } else if (ty < py) {
                sy = -1; // Si el objetivo esta abajo, paso es -1
                } // Si ty == py, sy se queda en 0
                // Fin del calculo de pasos 

                int x = px, y = py; // Coordenadas temporales para dibujar el tramo

                // Dibuja el tramo paso a paso hasta el objetivo (tx, ty)
                while (x != tx || y != ty) {
                    // Mueve primero en X, luego en Y (movimiento en L)
                    if (x != tx) {
                        x += sx;
                    } else if (y != ty) {
                        y += sy;
                    }

                    // Dibuja un punto '.' si la nueva posicion esta en el grid y vacia
                    if (y >= 0 && y <= GH && x >= 0 && x <= W) {
                        if (grid[y][x] == ' ') {
                            grid[y][x] = '.';
                        }
                    }
                }
                // Actualiza la posicion de partida para el siguiente tramo
                px = tx;
                py = ty;
            }
        }
    }

    // Dibujar monstruos (1..9 para los primeros 9, 'M' para los demas)
    for (int i=0;i<M;i++) if (monsters[i].a.alive){
        int x=monsters[i].a.x, y=monsters[i].a.y;
    if (y>=0 && y<=GH && x>=0 && x<=W){
            char mc = (monsters[i].id<10)?('0'+monsters[i].id):'M';
            grid[y][x] = mc;
        }
    }

    // dibujar heroes (A..Z para los primeros 26 heroes, 'H' para los demas)
    for (int h=0; h<H; ++h){
        Hero *hh = &heroes[h];
    if (!hh->a.alive) continue;
    int x=hh->a.x, y=hh->a.y;
    if (y>=0 && y<=GH && x>=0 && x<=W){
            char c;
            if (h < 26) c = (char)('A' + h);
            else c = 'H';
            grid[y][x] = c;
        }
    }

    // header
    if (title) printf("%s\n", title);
    printf("Grid %dx%d   Tick %d\n", W, GH, tick);

    // top border
    printf("   +"); for(int x=0;x<=W;x++) printf("-"); printf("+\n");

    //filas (se dibujan de arriba hacia abajo, para que el eje Y se vea creciendo hacia arriba)
        for (int y=GH; y>=0; --y){
        printf("%3d|", y);
        for (int x=0; x<=W; ++x) putchar(grid[y][x]);
        printf("|\n");
    }

    // border de abajo
    printf("   +"); for(int x=0;x<=W;x++) printf("-"); printf("+\n");

    // etiquetas eje x: mostrar digito de las decenas en multiplos de 10 y unidades en multiplos de 10
    printf("    ");
    for (int x = 0; x <= W; x++){
        if ((x % 10) == 0){
            int tens = (x / 10) % 10;
            printf("%c", (char)('0' + tens));
        } else {
            putchar(' ');
        }
    }
    printf("\n    ");
    for (int x = 0; x <= W; x++){
        /* muestra el digito de las unidades para cada columna (se repite de 0 a 9) */
        putchar((char)('0' + (x % 10)));
    }
    printf("\n");

    // legend and state
    printf("Leyenda: A..Z=Heroes, 1..9=Monsters (id), '.'=Heroe camino planeado\n");
    for (int h=0; h<H; ++h){
        Hero *hh = &heroes[h];
        printf("HERO%02d HP=%d at (%d,%d)%s\n",
               h+1, hh->a.hp, hh->a.x, hh->a.y, hh->engaged ? " [PELEANDO]" : "");
    }
    for (int i=0;i<M;i++){
    const char *life = monsters[i].a.alive ? "VIVO" : "MUERTO";
    const char *alrt = (monsters[i].a.alive && monsters[i].alerted) ? "ALERTA" : "";
    printf("M%02d at (%d,%d) HP=%d %s %s\n",
           monsters[i].id, monsters[i].a.x, monsters[i].a.y,
           monsters[i].a.hp,
           life,
           alrt);
    }

    for (int y=0;y<=GH;y++) free(grid[y]);
    free(grid);
}

// ----------------------------- Main ------------------------------
int main(int argc, char **argv){
    if (argc<2){
        fprintf(stderr, "Usage: %s <config.txt> [tick_us] [--ascii] [--ascii-only]\n", argv[0]);
        fprintf(stderr, "Examples:\n  %s config.txt 20000 --ascii\n  %s config.txt --ascii-only\n", argv[0], argv[0]);
        return 1;
    }

    // analizar los args finales: un numero se asigna a tick_us; las flags se asignan a ascii
    for (int i=2;i<argc;i++){
        if (strcmp(argv[i], "--ascii")==0) ascii_live=1;
        else if (strcmp(argv[i], "--ascii-only")==0) ascii_only=1;
        else if (isdigit((unsigned char)argv[i][0])) tick_us = atoi(argv[i]);
    }

    if (load_config(argv[1])!=0){
        fprintf(stderr, "Failed to load config\n");
        return 1;
    }

    printf("Grid %dx%d, Heroes=%d, Monsters=%d\n", G.width, G.height, H, M);

    if (ascii_only){
        ansi_clear();
        render_ascii_grid(0, "Escenario inicial (ASCII)");
        return 0;
    }

    // Threads + barrier
    int parties = 1 + H + M; // supervisor + heroes + monsters
    barrier_init(&tick_barrier, parties);
    barrier_init(&tick_barrier2, parties);

    // create hero threads
    for (int h=0; h<H; ++h){
        if (pthread_create(&heroes[h].th, NULL, hero_thread, (void*)(intptr_t)h)!=0){ perror("pthread_create(hero)"); return 1; }
    }
    for (int i=0;i<M;i++){
        if (pthread_create(&monsters[i].th, NULL, monster_thread, (void*)(intptr_t)i)!=0){
            perror("pthread_create(monster)"); return 1;
        }
    }

    // Supervisor loop
    int tick=0;
    for(;;){
    // Fase 1: esperar a que los actors terminen las acciones de este tick
    barrier_wait(&tick_barrier);

    pthread_mutex_lock(&world_mtx);
    bool monsters_alive = any_monster_alive();
    bool all_heroes_at_goal = true;
    bool any_hero_alive = false;

    for (int h = 0; h < H; ++h) {
        if (heroes[h].a.alive) {
            any_hero_alive = true; // al menos uno esta vivo

            // Solo consideramos el progreso de heroes VIVOS
            if (heroes[h].path_idx < heroes[h].path_len) {
                all_heroes_at_goal = false; // un heroe vivo aun no llega
            }
        }
    }

    int dummy = -1;
    bool combat_now = false;
    for (int h=0; h<H; ++h){
        if (any_monster_alive_in_range(heroes[h].a.x, heroes[h].a.y, heroes[h].a.attack_range, &dummy)) { combat_now = true; break; }
    }

    if (ascii_live){
        ansi_clear();
        render_ascii_grid(tick, "Simulacion - Vista ASCII");
    } else {
        print_state(tick);
    }
    // verificar condiciones de finalizacion
    if (!any_hero_alive){
        printf("\n>>> Todos los heroes murieron en el tick %d. GAME OVER.\n", tick);
        simulation_over = 1;
    } else if (all_heroes_at_goal && !combat_now){
        printf("\n>>> Todos los heroes alcanzaron sus objetivos en el tick %d. %s\n",
               tick, monsters_alive ? "Los monstruos permanecen, pero los heroes terminaron sus caminos." : "Todos los monstruos fueron eliminados.");
        simulation_over = 1;
    } else if (!monsters_alive){
        printf("\n>>> TODOS LOS MONSTRUOS MUERTOS en el tick %d.\n", tick);
        simulation_over = 1;
    }
    pthread_mutex_unlock(&world_mtx);
    // Fase 2: permitir que los actors observen simulation_over antes de comenzar un nuevo tick
    // El supervisor establece simulation_over mientras sostiene world_mtx, luego
    // espera en la segunda barrera para liberar a los actors en el siguiente ciclo.
    barrier_wait(&tick_barrier2);

    if (simulation_over) break;
    tick++;
    }

    // Join threads
    for (int h=0; h<H; ++h) pthread_join(heroes[h].th, NULL);
    for (int i=0;i<M;i++) pthread_join(monsters[i].th, NULL);

    for (int h=0; h<H; ++h) free(heroes[h].path);
    free(monsters);
    free(heroes);
    return 0;
}
