# Core PS4 di Lumen

> Core-only source export, 23 October 2026: the Lumen dashboard, Java/Qt/UIKit
> application UI and application packaging described in the original integration
> notes below are not included. Use the repository-root README for the export
> scope and dependency bootstrap. Runtime input/IME and in-game guest dialog
> support remain part of the core. The `lumen_dashboard` target is not provided.

Lumen è un porting ottimizzato per Android, iOS e Mac (Apple Silicon). Questo core deriva da
[shadPS4](https://github.com/shadps4-emu/shadPS4), attraverso il fork
[shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64) al pin
registrato in [UPSTREAM.json](UPSTREAM.json). I copyright originali e le
dichiarazioni `GPL-2.0-or-later` restano conservati; l’integrazione seleziona
GPL versione 2. Le singole componenti mantengono le proprie licenze.
Crediti Kyty e KytyPS5, mappa delle licenze e limiti della distribuzione sono
nel [documento di progetto](../../LICENSES.md).

La dashboard Lumen mantiene il protocollo di avvio gestito; il runtime PS4
è `lumen_gen4_runtime` su desktop e `liblumen_android_gen4.so` su Android.
Il motore PS5 conserva interfacce e processo separati.

Android ARM64 e macOS Apple Silicon usano l'engine FEX diretto al pin
`f2b679f6028ce1c38875233aecfcf5d3f8ebecec`, gli stessi servizi del core e
il tracciamento GPU tramite protezione delle pagine e fault, con readback
su richiesta. Android richiede pagine host da 4 KiB e rifiuta 16 KiB;
macOS aggrega i watcher logici da 4 KiB nelle pagine host da 16 KiB.
La sorgente FEX fissata in `source/3rdparty/fex-gen4` resta immutata;
macOS prepara una copia separata con il solo overlay host Darwin
(`cmake/direct_fex`), per MAP_JIT, W^X per thread e API native.
Il percorso diretto accetta solo FEX: una selezione esplicita di un altro
traduttore viene rifiutata, senza fallback.
Il core Gen4 non collega l'adattatore CPU precedente, la sua cache delle
traduzioni, le sue epoche di esecuzione o gli snapshot GPU espliciti.

macOS usa il backend Metal nativo. Android usa il driver Vulkan selezionato
dalla dashboard e un namespace SDL3 distinto dal runtime PS5. Il motore PS5
mantiene il suo grafo CPU separato.

Preparare le dipendenze prima del build (CMake 3.25+, Ninja, Python 3.12+ su macOS):

```sh
python3 source/gen4/tools/prepare_dependencies.py --platform macos
# Per Android usare --platform android.
```

Il build principale espone `lumen_gen4_runtime` e `lumen_dashboard`.
Il wrapper `tools/build_runtime.py` compila core e CPU in un grafo
indipendente; su macOS importa dal build principale l’adattatore Metal. Le variabili
CMake `LUMEN_GEN4_DEPS_DIR`, `LUMEN_GEN4_FULL_BUILD_DIR` e
`LUMEN_GEN4_FFMPEG_PREBUILT_DIR` selezionano cache e dipendenze.
La baseline macOS è arm64 nativa con deployment target 12.0.
I wrapper macOS accettano anche la variabile d'ambiente
`LUMEN_GEN4_FFMPEG_PREBUILT_DIR`, con il prefisso del pacchetto FFmpeg statico
arm64 contenente `include/libavcodec` e `lib/libavcodec.a`.

Su Android il flavor `fexLab` compila dashboard, PS4 e PS5. La proprietà
Gradle `lumenGen4DepsDir` seleziona le dipendenze; `lumenGen4BuildDir` può
selezionare un build indipendente dedicato alla variante. Non condividere
quel percorso tra build Android concorrenti o con ABI/STL differenti.
`-PlumenGen4BuildType=Release` compila il core PS4 in Release anche con
`assembleFexLabDebug`, mantenendo disponibili gli strumenti diagnostici
dell'app. Senza questa proprietà il core eredita il tipo di build dell'app.
Usare una directory dedicata, per esempio
`-PlumenGen4BuildDir=/percorso/assoluto/gen4-android-release`.
L'audit APK deve ricevere la stessa directory con `--runtime-build`;
verifica tipo di build, grafo compilato e identità della libreria inclusa.
Il pacchetto richiede API 35 e segmenti ELF compatibili con pagine da 16 KiB.

La sessione legge una sola root estratta, verifica ELF e SFO e separa i dati
scrivibili dal contenuto. Il profilo `known-system` ammette soltanto i moduli
previsti dal contratto della dashboard. Non vengono caricati profili per gioco,
patch automatiche o cartelle sorelle. I notice originali e i pin delle
dipendenze sono conservati in `../licenses/gen4` e nei pacchetti applicativi.

I test `lumen_gen4_guest_runtime`, `lumen_gen4_admission`,
`lumen_gen4_ime_bridge`, `lumen_gen4_virtual_input` e
`lumen_gen4_exception_boundary` verificano il confine CPU e il protocollo host.
`lumen_gen4_graphics_contract` avvia il renderer completo e presenta un frame.
I test Metal nel build principale verificano
sincronizzazione, rendering, indirizzi buffer e lettura per campione.
Le prove sintetiche non attestano la compatibilità di un gioco specifico.

Su desktop il contatore FPS è visibile all'avvio e F10 ne cambia la visibilità.
Su macOS i mapping tastiera predefiniti includono Invio/Z/N/tastierino 2
per Cross, X/B/tastierino 6 per Circle, Tab per Options e frecce per il D-pad.
WASD controlla lo stick sinistro e IJKL quello destro. Le configurazioni
personalizzate restano conservate; vengono aggiornati solo i file identici
ai precedenti default generati. La perdita del focus rilascia i tasti premuti.
