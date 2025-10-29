## EXTRA Conceptos Utilizados

### Multithreading 
- **Concepto**: Se crean múltiples hilos concurrentes: uno por cada héroe, uno por cada monstruo, más el hilo principal que actúa como **supervisor**. El **scheduler** del SO administra cómo comparten CPU.
- **Evidencia en código**:
  - Uso de **POSIX Threads** (`pthread.h`).
  - `pthread_t th;` en `Hero` y `Monster`.
  - `pthread_create()` para lanzar `hero_thread` y `monster_thread`.
  - `pthread_join()` en `main` para esperar su finalización.
- **Implicación de SO**: se apoya en **multiprogramación** y **planificación preventiva** (preemptive scheduling).

###  Sincronización
- **Problema**: Héroes y monstruos comparten/alteran **estado global** (arreglos `heroes`/`monsters`). Sin sincronización habría **race conditions**.
- **a) Mutex (exclusión mutua)**
  - `pthread_mutex_t world_mtx` protege las **secciones críticas** durante `hero_act_index()` / `monster_act()`.
- **b) Variables de condición**
  - En la implementación de **barrera**: los hilos **esperan** sin *busy-wait* con `pthread_cond_wait()` y se **despiertan** con `pthread_cond_broadcast()`.
- **c) Barreras (implementación propia)**
  - `barrier_t` + `barrier_init()` + `barrier_wait()` crean un **punto de encuentro** por tick.
  - Se usa **doble barrera** (`tick_barrier`, `tick_barrier2`) para separar “actuar” de “decidir/terminar”.

###  Sincronización POSIX 
- **Concepto**: API POSIX para mutex y condvars.
- **Evidencia**: llamadas `pthread_mutex_*` y `pthread_cond_*` tal como en los ejemplos del capítulo.  
  (No se usan semáforos POSIX en esta versión.)

### Evitar interbloqueo 
- **Concepto**: *Deadlock* aparece cuando hay **espera circular** por múltiples locks.
- **Razón de seguridad aquí**:
  - Se usa **un solo mutex global** (`world_mtx`) para el mundo ⇒ no hay jerarquías de locks contradictorias.
  - La barrera **libera** su mutex interno al esperar en la cond var, evitando bloqueos permanentes.
  - El **supervisor** decide `simulation_over` **entre** barreras; los actores siempre alcanzan la misma fase ⇒ no hay *livelock*.


---
