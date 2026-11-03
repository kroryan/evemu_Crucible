# PlayerBots: plan de acción

## Objetivo

Completar una población configurable de hasta 40–60 personajes persistentes que
use las mecánicas normales del servidor. Minería, combate, industria, contratos,
mercado, inventario, ISK, pérdidas y recompensas deben validarse y persistirse
por los servicios de EVEmu. El LLM solo redacta mensajes: nunca decide ni
fabrica resultados del juego.

Este plan complementa [PlayerBots.md](PlayerBots.md), que describe la base
actual. Compilar no basta para cerrar un hito: cada uno exige su criterio
funcional.

## Estado actual observado

Ya hay cuentas y personajes dedicados, login virtual, persistencia básica, un
ciclo MINER, conducta simple de RAT_HUNTER y TRADER, comandos `/bot`, órdenes por
prefijo, alta en flota/corporación, contratos de mensajería y chatter opcional.
La configuración permite hasta 60 bots. La creación ya puede dar ISK inicial,
skills y fitting básico sin pasar por el tutorial de un jugador.

Hay bloqueos funcionales: HAULER, MISSION_RUNNER e INDUSTRIALIST todavía usan el
fallback minero; TRADER solo cruza ofertas existentes; contratos usan SQL
directo; el horario y el límite de riqueza se configuran pero no se aplican al
ciclo; el presupuesto procesa siempre los primeros bots por ID; y el brain
vuelve a posponer `respawnAt` mientras el bot está en cápsula.

## Reglas de compatibilidad y datos

- Mantener Crucible 7.31, build 360229 y MachoNet 320. No reintroducir formatos
  de respuesta de cliente que impidan login o selección de personaje.
- Revertir PR #323 con un commit inverso y adaptar sus dependencias posteriores.
  No usar `reset --hard`, reescritura de historial ni force-push.
- No borrar/recrear MariaDB, cuentas de jugadores, datos del market ni volúmenes
  Docker. Migraciones de bots solo aditivas.
- No ejecutar `clean_db.sql`, `seed_market` ni scripts de recreación de datos.
  Integrar `MarketBotMgr` existente sin sustituir sus órdenes.
- El bootstrap puede saltarse el tutorial y otorgar una vez ISK limitado,
  skills, nave y módulos reales. Después, ingresos, compras y pérdidas siguen
  las mecánicas normales y respetan un máximo de riqueza.
- No escribir outcomes directamente: usar servicios de juego para combate,
  contratos, mercado, agentes, fabricación y PI. No eludir crimen ni reglas de
  seguridad para órdenes PvP.

## Fases de trabajo

### 0. Retirar #323 y recuperar el login completo

- [ ] Revertir #323 conservando los cambios propios de bots y los fixes de
  compatibilidad Crucible que sigan siendo necesarios.
- [ ] Compilar y reconstruir la imagen del servidor; desplegar solo
  `evemu-server`, sin recrear ni reiniciar la DB.
- [ ] Validar login, lista, selección, `checking navigation systems`, carga del
  mundo y logout con Crucible 7.31.
- [ ] Confirmar MachoNet 320 en logs y revisar las llamadas de servicio que el
  cliente hace tras seleccionar el personaje.

**Salida:** el jugador entra al mundo en una sesión nueva y se conservan los
datos de DB y market existentes.

### 1. Estabilizar 40–60 bots

- [ ] Repartir el presupuesto con cursor round-robin y límite de acciones/tiempo
  para que ningún bot de ID alto quede sin procesar.
- [ ] Escalonar login virtual y primer movimiento para evitar una oleada de
  entidades y warps al arrancar con 60 bots.
- [ ] Corregir muerte y respawn: fijar `respawnAt` una sola vez; usar clone,
  cápsula y rookie ship del flujo normal; no reembolsar pérdidas.
- [ ] Recuperar estados tras reinicio validando nave, estación, sistema y belt.
- [ ] Reducir escrituras repetidas: persistir cambios o guardar en intervalos
  acotados, nunca bloquear el main loop con trabajo lento.
- [ ] Aplicar horarios y presencia virtual, límites válidos y pausa/reanudación.
- [ ] Exponer estado, errores, consultas, tick, wallet, cargo y motivo de pausa.

**Salida:** ningún bot se queda hambriento de ticks; 1, 10, 40 y 60 bots no
provocan pausas sostenidas del loop y retoman su trabajo tras un reinicio.

### 2. Órdenes, chat, corporación y flota

- [ ] Añadir selector de bot o grupo; hoy varias acciones se envían a todos los
  bots online.
- [ ] Completar `/bot order <bot|grupo> <acción>` para movimiento, docking,
  minería por belt/mineral, ataque de una entidad visible y producción por
  receta/cantidad. Informar aceptación, progreso y causa de fallo.
- [ ] Añadir órdenes dirigidas para mercado, misión, PI, fabricación y contratos.
- [ ] Hacer invitación/aceptación de corp con los servicios y permisos reales;
  soportar bots en corp de jugadores y una corp de bots que invite jugadores.
- [ ] Usar `FleetService` para invitaciones, aceptación, altas y salidas;
  respetar capacidad y permisos.
- [ ] Dirigir mensajes por local/corp/flota/privado, resolver menciones sin
  responder desde un bot arbitrario y evitar spam.
- [ ] Documentar solo comandos implementados en `doc/commandlist`.

**Salida:** el jugador puede ordenar bots concretos y ver eventos/membresías
coherentes en cliente y DB.

### 3. Minería y economía real

- [ ] Completar MINER: cambiar de asteroide agotado, controlar ciclos y bodega,
  volver/dockear y conservar mineral si no existe una compra ejecutable.
- [ ] Refinar solo mediante el servicio de refinado, con sus costes e impuestos.
- [ ] Completar TRADER con órdenes reales colocadas/canceladas vía `MarketMgr`,
  fills, wallet, tasas e historial; comprar naves, módulos y munición reales.
- [ ] Integrar `MarketBotMgr` como maker de fondo, sin resetear órdenes existentes.
- [ ] Aplicar `maxISK` a ingresos y transferencias. Las únicas fuentes serán
  bootstrap limitado, bounties, recompensas reales y ventas liquidadas.
- [ ] Hacer bootstrap de cuentas, skills, fittings e ISK idempotente; no crear
  órdenes gratuitas ni resembrar el mercado.

**Salida:** cada mineral, item, ISK, impuesto, fill y pérdida persiste y se puede
rastrear después de reiniciar el servidor.

### 4. Profesiones

Implementar y activar cada rol por separado. Mientras use otro ciclo, debe
mostrarse pendiente y no como profesión terminada.

- [ ] **MINER:** ciclo completo anterior, con fit y skills válidos.
- [ ] **HAULER:** contrato/carga real, ruta por stargates con autopilot, entrega,
  pago, bodega, ruta bloqueada y pérdida de nave.
- [ ] **RAT_HUNTER:** NPC en belt/anomalía, lock y armas reales, bounty normal,
  loot real, retorno y huida según daño/riesgo.
- [ ] **MISSION_RUNNER:** pedir/aceptar misión de agente, cumplir objetivos y
  cobrar mediante `AgentMgrService`; nunca marcar misión desde el brain.
- [ ] **INDUSTRIALIST:** blueprint, skills, materiales y wallet; instalar,
  ejecutar y recoger jobs mediante manufacturing real.
- [ ] **PLANETARY INDUSTRY:** crear command center, extractores, links/rutas,
  ciclos y transferencias mediante los servicios de PI.
- [ ] **TRADER:** market maker/taker con capital propio y límites antiinflación.
- [ ] **PIRATE/PVP opcional:** después de rat hunter; respetar security,
  standings, crimen, lock, módulos y killmails. Sin ataque autónomo salvo opt-in.

**Salida:** cada rol tiene flujo de éxito y fallo usando las mismas rutas de
servicio que el jugador, y persiste correctamente.

### 5. Skills y comportamiento orgánico

- [ ] Perfiles de skills por rol y subespecialidad; validar prerequisitos al
  ejecutar órdenes, no usar la profesión como etiqueta cosmética.
- [ ] Fittings iniciales coherentes mediante inventario real; equipo perdido no
  reaparece gratis y se reemplaza con fondos u orden explícita.
- [ ] Repartir bots por estaciones/sistemas con mayoría en highsec; viajar entre
  sistemas por stargates, no teletransportarse.
- [ ] Aplicar horarios, pausas y preferencias reproducibles por configuración.
- [ ] Reaccionar a daño/agresión: cancelar tarea, huir, pedir ayuda en corp/flota
  y reportar en local sin spam.
- [ ] Verificar presencia en local, overview, corp, fleet, market y killmails por
  los flujos normales del servidor.

**Salida:** hay variedad de perfiles y rutas, pero todas las consecuencias son
reproducibles y autoritativas del servidor.

### 6. Chat y LLM

- [ ] Mantener fallback determinista, cooldown, probabilidad y límites por canal.
- [ ] Procesar HTTP fuera del hilo de mundo, con timeout, cola limitada y parada
  ordenada; descartar respuestas tardías de bots desconectados.
- [ ] Soportar HTTPS con TLS verificado y JSON robusto para Ollama y endpoints
  OpenAI-compatible. No registrar api keys ni secretos.
- [ ] Enviar contexto de personalidad sin secretos ni mensajes privados no
  autorizados. El bot no afirmará acciones que el juego no confirmó.
- [ ] El LLM nunca elige blancos, rutas, precios, ISK, recetas ni outcomes.

**Salida:** caída o ausencia del proveedor no altera la simulación ni bloquea el
servidor; el fallback sigue contestando.

### 7. Aceptación y despliegue

- [ ] Compilar en C++17 y desplegar la imagen reconstruida del servidor.
- [ ] Revalidar login Crucible antes y después de cada cambio de protocolo.
- [ ] Comprobar en DB personajes, skills, wallet, nave, fitting, cargo, market,
  contratos, jobs, PI, kills y estados tras reinicio.
- [ ] Cubrir fallos: sin belt, sin buy order, wallet bajo, nave destruida,
  reinicio en warp, corp/flota llenas y agente/blueprint/planeta inválido.
- [ ] Medir loop, consultas, memoria y sockets con 10/40/60 bots.
- [ ] Revisar Compose y desplegar solo el contenedor `server`; no borrar volúmenes
  ni ejecutar scripts de limpieza/seed.
- [ ] Pushear commits pequeños y reversibles después de compilar y revisar el
  hash remoto.

## Secuencia

1. Retirar #323 y recuperar navegación/login completo.
2. Corregir scheduler, respawn y persistencia.
3. Terminar órdenes dirigidas, corp/flota y chat estructurado.
4. Completar minería, market y límites económicos.
5. Añadir HAULER, RAT_HUNTER, MISSION_RUNNER, INDUSTRIALIST/PI y TRADER;
   PIRATE/PVP va al final y es opcional.
6. Añadir realismo, LLM seguro y aceptación a 40–60 bots.

Cada profesión se habilita solo al pasar su criterio. Los bots reciben al
provisionarse skills, ISK, nave y módulos iniciales configurados, pero su
actividad posterior usa servicios reales.
