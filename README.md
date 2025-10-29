# threaded-grid-simulator

## Resumen
Pequeña simulación en C, multi-hilo, donde uno o varios héroes recorren rutas predefinidas en una cuadrícula mientras monstruos se desplazan, se alertan, persiguen y atacan.  
El programa usa **pthreads** para concurrencia y una **barrera en dos fases** (supervisor + actores) para sincronizar *ticks* y evitar condiciones de carrera cuando el supervisor decide terminar la simulación.

---

## Funciones principales
- Soporta **múltiples héroes** (`HERO_COUNT`) y **múltiples monstruos** (`MONSTER_COUNT`).
- Los **héroes** siguen listas independientes de *waypoints* `PATH (x,y)` y **se detienen a combatir** cuando hay un monstruo en rango de ataque.
- Los **monstruos** tienen radio de **visión**; al ver a un héroe **se alertan**, **alertan a vecinos** y, si están alertados, **persiguen** por distancia de **Manhattan** y **atacan** si están en rango.
- **Barrera en dos fases por tick**:
  - **Fase A**: todos los actores completan sus acciones del tick.
  - **Supervisor**: inspecciona mundo, imprime estado/ASCII y decide si `simulation_over`.
  - **Fase B**: todos observan la decisión y pasan (o terminan).
- **Visualización ASCII** y **estado por tick** disponibles.

---

## Archivos
- `doom_sync_sim.c` — código fuente principal (parser, simulación, renderizado, hilos).
- `config.txt`, `config1.txt` — ejemplos de configuración usados para pruebas.

---

## Compilación
Compilar con gcc (se requiere pthreads):
```bash
gcc -O2 -std=c11 -pthread -o doom_sim doom_sync_sim.c
```


---

## Ejecución
```bash
./doom_sim config.txt                     # Ejecuta la simulación (imprime estado por tick)
./doom_sim config.txt 400000 --ascii       # Animación con ASCII (~0.4 s entre ticks)
./doom_sim config.txt --ascii-only        # Muestra la vista ASCII tick inicial
```

---

## Formato de configuración
- `GRID_SIZE W H`
- `HERO_COUNT N`
- `HERO_i_HP`, `HERO_i_ATTACK_DAMAGE`, `HERO_i_ATTACK_RANGE`
- `HERO_i_START X Y`
- `HERO_i_PATH (x1,y1) (x2,y2) ...`
- `MONSTER_COUNT M`
- `MONSTER_i_HP`, `MONSTER_i_ATTACK_DAMAGE`, `MONSTER_i_VISION_RANGE`, `MONSTER_i_ATTACK_RANGE`
- `MONSTER_i_COORDS X Y`

---

## Validaciones y notas
- El parser **valida** el tamaño de la cuadrícula y exige **al menos un héroe**.
- Verifica que `HP`, `ATTACK`, `ATTACK_RANGE`, `VISION_RANGE` sean **no negativos**.
- Comprueba que las **coordenadas de inicio** y todos los **waypoints del `PATH`** estén dentro de `[0..width]` y `[0..height]`.
- Cuando una entidad llega a `HP <= 0` su `HP` se **satura a 0** y su bandera `alive` pasa a `false`.  
  En la impresión, **no** se muestra `ALERTED` si está `DEAD`.
- El programa **libera la memoria** asignada a héroes (incluyendo `path`) y monstruos al finalizar.

---

## Comportamiento y terminación
La simulación termina cuando se cumple cualquiera de las condiciones:
- **Todos los héroes están muertos**, o
- **Todos los héroes** alcanzaron sus rutas **y no hay combate** en curso, o
- **Todos los monstruos están muertos**.

---

## Flujo por tick
1. **Actúan héroes** (bajo `world_mtx`): si hay enemigo en rango ⇒ **atacan** y **no se mueven**; si no ⇒ **avanzan** 1 paso hacia su *waypoint*.
2. **Actúan monstruos** (bajo `world_mtx`): si ven héroe ⇒ **alertan vecinos**; si alertados ⇒ **persiguen** al **héroe más cercano** (Manhattan) y **atacan** si están en rango.
3. **Barrera Fase A**: todos los hilos de actores llegan aquí.
4. **Supervisor**: imprime estado/ASCII, evalúa condición de término y, si aplica, fija `simulation_over`.
5. **Barrera Fase B**: todos leen la decisión y continúan o salen.

---

# Código explicado paso a paso

## 1. Preparación (includes y definiciones)
- `#include <stdio.h>`, `#include <stdlib.h>`, `#include <string.h>`, `#include <pthread.h>`, `#include <stdbool.h>`, `#include <stdint.h>`, `#include <ctype.h>`, `#include <unistd.h>`, `#include <time.h>`: habilitan las bibliotecas estándar y POSIX necesarias para entrada/salida, gestión de memoria, cadenas, concurrencia (pthreads), tipos básicos y temporización.
- `#define _XOPEN_SOURCE 700`: solicita al compilador funciones y constantes de la familia POSIX/XSI necesarias para la temporización y sincronización en sistemas tipo Unix.
- `#define MAX_PATH_POINTS`: fija el número máximo de *waypoints* que puede contener la ruta de un héroe.

## 2. Utilidades
- `abs_i(int x)`: devuelve el valor absoluto de un entero.
- `manhattan(int x1, int y1, int x2, int y2)`: calcula la distancia de Manhattan entre dos puntos; resulta idónea para estrategias de persecución con movimientos ortogonales.
- `sleep_us(int us)`: implementa una pausa en microsegundos mediante `nanosleep`, evitando *busy-waiting*.
- `ansi_clear(void)`: emite secuencias ANSI para limpiar la pantalla, útil en la visualización ASCII.

## 3. Barrera de sincronización
Se implementa una barrera reutilizable que coordina a todos los hilos por *tick*.

- `typedef struct barrier_t`:
  - `pthread_mutex_t mtx`: exclusión mutua para proteger el estado interno de la barrera.
  - `pthread_cond_t  cv`: variable de condición para dormir/despertar hilos sin busy-wait.
  - `int parties`: número total de participantes esperados en cada ciclo.
  - `int count`: contador decreciente de llegadas en el ciclo actual.
  - `int cycle`: identificador de generación que evita despertar prematuro en ciclos subsiguientes.
- `barrier_init(barrier_t* b, int parties)`: inicializa mutex, condición y contadores con el número de participantes.
- `barrier_wait(barrier_t* b)`: protocolo de espera:
  1. Bloquea `mtx` y captura `cycle` local.
  2. Decrementa `count`. Si llega a cero, el hilo actual es el último en alcanzar la barrera: incrementa `cycle`, restablece `count` a `parties` y realiza `pthread_cond_broadcast(&b->cv)` para liberar a todos.
  3. Si no es el último, espera en `pthread_cond_wait(&b->cv, &b->mtx)` dentro de un bucle `while (cycle == b->cycle)` para evitar *spurious wakeups*.
  4. Desbloquea `mtx` y retorna.

Se utilizan **dos** barreras por *tick* (`tick_barrier` y `tick_barrier2`) para separar **fase de acción** (actores actualizan estado) de **fase de decisión** (supervisor inspecciona y eventualmente termina la simulación).

## 4. World types
- `Point`: par ordenado `(x, y)`.
- `Grid`: dimensiones de la cuadrícula (`width`, `height`).
- `Actor`: atributos comunes a héroes y monstruos: posición, puntos de vida (`hp`), potencia de ataque (`attack`), alcance (`attack_range`) y estado de vida (`alive`).
- `Hero` (especializa `Actor`):
  - `path` (arreglo de `Point`), `path_len`, `path_idx` para seguimiento de ruta.
  - `engaged`: indica si está en combate (en cuyo caso no avanza en la ruta).
  - `pthread_t th`: hilo asociado.
- `Monster` (especializa `Actor`):
  - `id` identificador, `vision` (radio de detección), `alerted` (estado de alerta), `pthread_t th`.

## 5. Global State
- `G, heroes, monsters, H, M`: representación del mapa y colecciones de entidades; su visibilidad `static` limita el alcance al archivo fuente.
- `simulation_over` (`volatile int`): bandera de terminación observada por todos los hilos.
- `world_mtx`: mutex global que protege las **secciones críticas** (lectura/modificación del estado del mundo) y evita condiciones de carrera.
- `tick_barrier`, `tick_barrier2`: barreras de sincronización en dos fases por *tick*.
- Parámetros de visualización y temporización: `tick_us`, `ascii_live`, `ascii_only`, `ascii_show_path`.

## 6. Funciones auxiliares
- `any_monster_alive_in_range(x, y, range, *out_idx)`: busca un monstruo vivo dentro del alcance de ataque y reporta su índice.
- `any_monster_alive()`: determina si aún existen monstruos vivos.
- `alert_neighbors(src_idx)`: propaga el estado de alerta a monstruos dentro del radio de visión del emisor.

## 7. Lógica de acciones
- `hero_act_index(int h)`:
  - Si hay un monstruo vivo dentro de `attack_range`, el héroe entra en combate (`engaged = true`), aplica daño y **no** avanza en la ruta.
  - En ausencia de combate, progresa un paso ortogonal hacia el siguiente *waypoint* y actualiza `path_idx` al alcanzarlo.
- `monster_act(int i)`:
  - Selecciona el héroe vivo más cercano (distancia de Manhattan).
  - Si el héroe está dentro de `vision` y el monstruo no estaba alertado, lo marca `alerted = true` y notifica a vecinos.
  - Si el héroe está dentro de `attack_range`, aplica daño; en caso contrario, si está alertado, avanza un paso hacia el objetivo.

## 8. Threads
- `hero_thread(void*)` y `monster_thread(void*)` repiten, por *tick*:
  1. Bloquean `world_mtx`, ejecutan su acción (`hero_act_index` / `monster_act`) y liberan `world_mtx`.
  2.  Pausa breve `sleep_us(tick_us)` para legibilidad.
  3. **Fase A**: `barrier_wait(&tick_barrier)`; garantiza que todos los actores completaron el *tick*.
  4. **Fase B**: `barrier_wait(&tick_barrier2)`; libera al supervisor para decidir y a los actores para observar `simulation_over` antes del siguiente ciclo.
  5. Si `simulation_over` es verdadero, terminan.

## 9. Parser
- Utilidades: `ltrim`, `rtrim_inplace` para normalización de líneas; `parse_path_points` para extraer *waypoints* tipo `(x,y)`.
- `load_config(const char* path)`:
  1. Establece valores por defecto del mundo.
  2. Recorre el archivo línea a línea, ignorando vacíos/comentarios (`#`).
  3. Procesa directivas (`GRID_SIZE`, `HERO_COUNT`, `HERO_i_*`, `MONSTER_COUNT`, `MONSTER_i_*`) con `strncmp`/`sscanf`.
  4. Reserva/ajusta memoria dinámica (`malloc/calloc/realloc`) para héroes y monstruos.
  5. Finaliza cargando valores por defecto en campos no provistos y validando rangos básicos.

## 10. Visualización
- `print_state(int tick)`: salida textual compacta con posiciones, HP y banderas relevantes por entidad.
- `render_ascii_grid(int tick, const char* title)`: representación en cuadrícula:
  1. Construye un *buffer* de caracteres y, opcionalmente, traza la ruta planificada con `'.'`.
  2. Coloca héroes (letras `A..Z`) y monstruos (ids `1..9` o `'M'`).
  3. Imprime marco, ejes y leyenda; libera la memoria temporal.

## 11. Función principal (`main`)
1. Valida argumentos y opciones (`tick_us`, `--ascii`, `--ascii-only`), y llama a `load_config`.
2. Si `--ascii-only`, renderiza el estado inicial y finaliza.
3. Inicializa **dos barreras** con `parties = 1 + H + M` (supervisor + actores).
4. Crea los hilos de héroes y monstruos (`pthread_create`).
5. **Supervisor** por *tick*:
   - Espera **Fase A** en `tick_barrier` (actores han completado el *tick*).
   - Con `world_mtx` adquirido, imprime estado/ASCII, evalúa condiciones de término y, en su caso, marca `simulation_over`.
   - Señaliza **Fase B** en `tick_barrier2` para que actores observen la decisión.
   - Sale si `simulation_over` se activó; si no, continúa al siguiente *tick*.
6. Sincroniza terminación (`pthread_join`) y libera recursos (`free`).



