# Eden r46d3 portable v0.0.1

Primera versión pública para Windows x64 basada en Eden 0.2.1.

## Biblioteca ampliada

- Trailer integrado por juego mediante YouTube Data API.
- Número de jugadores, género y fecha real de lanzamiento mediante metadatos en línea.
- Etiquetas y correcciones locales editables.
- Build ID efectivo calculado con la actualización instalada.
- Disponibilidad y descarga selectiva de cheats con coincidencia exacta de Title ID y
  Build ID.
- Comparación automática entre actualización instalada y última versión conocida.
- Columnas `Última partida`, `Agregado a Eden` y `Creado` con persistencia local.
- Cachés y colas de red para evitar consultas excesivas.

## Modo de trucos integrado

- Acceso con `ZL + ZR + Más` o `Ctrl + Shift + C`.
- Captura de referencia y pausa/reanudación segura del juego.
- Búsquedas de valor conocido y valor inicial desconocido.
- Int8/UInt8, Int16/UInt16, Int32/UInt32, Int64/UInt64, Float y Double.
- Comparaciones exacta, cambió, sin cambios, aumentó, disminuyó, mayor y menor.
- Alineación natural o exploración byte por byte.
- Sesiones persistentes por Title ID y Build ID, con deshacer escaneo.
- Visor de candidatos con región, offset relativo, tipo y valor actual.
- Selección individual, múltiple o total y escritura del mismo valor en lote.
- Verificación de escrituras, deshacer y restauración de valores originales de sesión.
- Congelamiento temporal a 12 Hz limitado a un candidato en modo seguro.
- Navegación completa con control y teclado numérico integrado.

## Portabilidad y privacidad

- La carpeta vacía `user` activa la configuración portable.
- El ZIP no contiene claves, firmware, juegos, DLC, partidas, tokens ni registros.
- Todos los datos generados permanecen dentro de la carpeta portable.

Consulta [README.md](README.md) para la guía completa y las advertencias de uso.
