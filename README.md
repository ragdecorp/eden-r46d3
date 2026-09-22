# Eden r46d3

Eden r46d3 es una modificación comunitaria de **Eden 0.2.1** para Windows x64. Amplía
la biblioteca con información útil por juego y añade un modo de trucos integrado que
permite buscar, comprobar y modificar valores de la memoria emulada sin utilizar una
aplicación externa.

> Proyecto no oficial. No está afiliado con Nintendo, Eden Emulator Project,
> CheatSlips, nlib.cc ni YouTube.

## Descargar

La edición portable compilada está en la sección
[Releases](https://github.com/ragdecorp/eden-r46d3/releases/latest). Descarga el ZIP,
extráelo en una carpeta con permisos de escritura y ejecuta `eden.exe`.

La distribución está deliberadamente limpia: **no incluye juegos, claves, firmware,
actualizaciones, DLC, partidas guardadas ni credenciales de servicios externos**. La
carpeta vacía `user` activa el modo portable y guarda toda la configuración junto al
ejecutable.

## Cambios de la biblioteca, columna por columna

El orden visual puede cambiar si el usuario mueve o restaura columnas.

| Columna | Funcionamiento |
| --- | --- |
| **Nombre** | Muestra icono, nombre y Title ID del juego. Conserva el comportamiento original de Eden. |
| **Trailer** | `Ver Trailer` busca un video corto y reproducible del juego mediante YouTube Data API y lo abre en un reproductor integrado. Requiere credenciales propias de YouTube. |
| **Jugadores** | Obtiene de nlib.cc el mínimo y máximo de jugadores. Puede editarse manualmente y el valor personalizado tiene prioridad sobre el dato en línea. |
| **Género** | Obtiene las categorías del catálogo en línea. También admite una corrección local manual. |
| **Etiquetas** | Etiquetas personales separadas por coma o punto y coma. Se guardan localmente y participan en el filtro de la biblioteca. |
| **Build ID** | Lee los primeros 16 caracteres del Build ID del NSO efectivo. Considera el ejecutable resultante después de aplicar la actualización instalada; por eso identifica la versión real que se va a ejecutar. |
| **Trucos** | Comprueba en segundo plano si CheatSlips tiene códigos para la combinación exacta de Title ID y Build ID. Puede mostrar `Comprobando`, `Disponible (n)`, `No disponible`, `Error de conexión` o `Sin comprobar`. Al pulsarla descarga el catálogo, permite elegir códigos y los instala en `user/load/<TITLE_ID>/.../cheats/<BUILD_ID>.txt`. |
| **Tipo de archivo** | Formato detectado por Eden, por ejemplo NSP o XCI. Función original. |
| **Tamaño** | Tamaño del archivo del juego. Función original. |
| **Tiempo de juego** | Tiempo acumulado por Eden. Función original. |
| **Última partida** | Registra fecha y hora únicamente cuando el juego termina de cargar correctamente. Los juegos existentes quedan inicialmente en blanco hasta su primera ejecución con esta versión. |
| **Agregado a Eden** | Registra cuándo un Title ID nuevo aparece por primera vez en la biblioteca. La instalación inicial crea una línea base y deja en blanco los juegos que ya existían, para no inventar fechas. |
| **Creado** | Fecha real de lanzamiento consultada en línea por Title ID; no utiliza la fecha del archivo NSP/XCI. |
| **Complementos** | Muestra la actualización y DLC instalados. Conserva la información local de Eden. |
| **Actualización** | Compara la versión instalada con el catálogo en línea. Estados principales: `Al día`, `Nueva: vNNNN | x.y.z`, `Versión local más nueva`, `Sin datos` y errores de conexión. Al colocar el cursor muestra versión instalada, disponible, fecha del catálogo y fuente. |
| **Compatibilidad** | Estado de compatibilidad de Eden cuando está habilitado. Función original. |

### Consultas en segundo plano y caché

- Metadatos de jugadores, género y fecha de lanzamiento: hasta dos solicitudes en
  paralelo; caché local durante 30 días.
- Catálogo numérico de actualizaciones: caché durante 24 horas. El nombre legible de
  la versión se conserva hasta 30 días.
- Disponibilidad de CheatSlips: una consulta cada cinco segundos para no sobrecargar
  el servicio; caché durante siete días y pausas automáticas ante errores o límites.
- El cuadro de búsqueda también encuentra texto de las columnas nuevas.

Los datos locales se guardan bajo `user/cache` y `user/config`. El historial de fechas
de la biblioteca se guarda en `user/config/game_library_history.json`.

## Trucos descargables de CheatSlips

La disponibilidad se puede consultar sin descargar los códigos. Para descargar se
necesita un token personal de CheatSlips:

1. Crea `user/config/cheats/cheatslips_token.json`.
2. Guarda tu token con este formato:

   ```json
   {
     "token": "TU_TOKEN_PERSONAL"
   }
   ```

3. Reinicia Eden, espera a que la columna indique `Disponible (n)` y pulsa la celda.
4. Selecciona uno o varios códigos y confirma la instalación.

Eden valida que el contenido tenga opcodes reconocibles y usa siempre la coincidencia
exacta **Title ID + Build ID**. El token es responsabilidad del usuario, no debe
subirse a GitHub y la cuota diaria pertenece a la cuenta de CheatSlips.

## Modo de trucos dentro del juego

Con un juego en ejecución, abre el panel con:

- Control: **ZL + ZR + Más**.
- Teclado: **Ctrl + Shift + C**.

Eden captura una imagen de referencia, pausa la emulación mientras el panel está
abierto y continúa el juego al cerrarlo. Conserva como máximo las 20 capturas más
recientes de cada Title ID y Build ID.

### Controles

- Cruceta o stick: navegar.
- A: confirmar o seleccionar.
- B: cerrar o regresar.
- El teclado numérico integrado permite introducir valores grandes, negativos o
  decimales según el tipo elegido.

### Búsqueda cuando conoces el valor

1. Abre el modo de trucos y elige **Sé el valor actual**.
2. Introduce el valor mostrado por el juego.
3. En opciones avanzadas selecciona el tipo de dato o usa **Automático (32/64-bit)**.
4. Ejecuta **Nuevo escaneo**.
5. Continúa el juego hasta que el valor cambie.
6. Vuelve a abrir el panel, elige **Continuar búsqueda anterior**, introduce el nuevo
   valor y ejecuta **Siguiente escaneo**.
7. Repite hasta obtener 500 candidatos o menos y abre **Ver candidatos**.

El modo automático prueba Int32, Int64, Float y Double. También pueden elegirse
manualmente Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float y Double.

### Búsqueda cuando desconoces el valor

1. Elige **No sé el valor** y crea la captura inicial de memoria.
2. Continúa el juego y provoca un cambio observable.
3. Regresa y selecciona la relación correcta: **Aumentó**, **Disminuyó**, **Cambió**,
   **Sin cambios**, **Mayor que**, **Menor que** o un valor exacto.
4. Ejecuta **Siguiente escaneo** y repite hasta reducir los resultados.

La alineación **Natural** es la recomendada. **Byte por byte** encuentra valores no
alineados, pero es más lenta y produce muchos más candidatos.

### Persistencia de la búsqueda

La búsqueda se guarda por Title ID y Build ID dentro de `user/cheats_studio`. Puede
cerrarse el panel, jugar y continuar en la siguiente apertura. **Deshacer escaneo**
recupera el conjunto anterior. Se admiten hasta 2,000,000 de candidatos guardados y el
visor se habilita al reducirlos a 500 o menos.

### Visor y modificación de candidatos

El visor muestra:

- dirección absoluta;
- región de memoria;
- offset relativo a la región;
- tipo de dato;
- valor del último escaneo;
- valor actual y estado.

Las regiones `Main` son potencialmente más estables. Heap, ASLR, alias y mapeos
normales suelen cambiar entre ejecuciones; una dirección encontrada no constituye por
sí sola un cheat persistente.

Se puede seleccionar una dirección, varias o todas y usar **Cambiar seleccionados**.
El mismo valor se escribe una sola vez en todas las seleccionadas después de una
confirmación explícita. Eden verifica las escrituras y guarda los valores originales
antes de modificar la memoria.

Funciones de seguridad:

- **Deshacer último cambio** restaura la escritura más reciente.
- **Restaurar cambios de la sesión** intenta devolver todos los valores originales
  respaldados durante esa ejecución.
- **Congelar seleccionados** mantiene temporalmente un valor a 12 Hz. El modo seguro
  exige exactamente un candidato validado para evitar bloquear controles o corromper
  el estado del juego.
- El congelamiento se desactiva al cerrar el juego y también puede detenerse
  manualmente.

Modificar memoria puede alterar lógica interna, bloquear controles o cerrar el juego.
Haz una copia de tus partidas y valida primero un solo candidato. El escáner no genera
automáticamente códigos persistentes para otra ejecución o Build ID.

## Credenciales opcionales para trailers

Guarda una clave propia de YouTube Data API en
`user/config/youtube/youtube_api_key.json`:

```json
{
  "api_key": "TU_API_KEY"
}
```

También se admite OAuth mediante `user/config/youtube/token_youtube.json`. Ninguna
credencial se incluye en la distribución ni debe confirmarse en Git.

## Requisitos y contenido no incluido

- Windows x64.
- GPU y controlador compatibles con Eden 0.2.1.
- Copias legales de tus juegos.
- Claves y firmware obtenidos de tu propia consola.

No solicites ni publiques claves, firmware, juegos, actualizaciones o DLC en este
repositorio.

## Código fuente y licencia

Este repositorio conserva el historial del proyecto Eden y se basa en la etiqueta
`v0.2.1`, commit `58c1e20ee5`. El código está disponible bajo GPL-3.0-or-later y las
licencias aplicables indicadas en `LICENSE.txt` y `LICENSES/`.

- Proyecto original: <https://git.eden-emu.dev/eden-emu/eden>
- Guía de compilación: [docs/Build.md](docs/Build.md)
- Cambios de esta versión: [RELEASE_NOTES_v0.0.1.md](RELEASE_NOTES_v0.0.1.md)

Eden, yuzu y los demás proyectos de los que deriva conservan sus créditos y avisos
originales.
