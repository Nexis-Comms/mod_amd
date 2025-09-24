# Asterisk app\_amd for FreeSWITCH (mod\_amd)

This module implements Asterisk-style Answering Machine Detection (AMD) for FreeSWITCH. It can be started from the **dialplan** as a non-blocking app **or** dynamically on an existing channel via a **UUID API command** (usable from fs\_cli and mod\_event\_socket).

---

## What’s new

* **`uuid_amd_detect <uuid> [key=val;...]`**: start AMD on a live channel by UUID (works from **fs\_cli** or **ESL/event socket**).
  The API invokes the same internal logic as the dialplan app (`amd_start_function`) and accepts the same optional parameters.
* **Custom event**: fires subclass **`amd`** with headers `AMD-Result` and `AMD-Cause` when a decision is made.
* **Execute-on hooks**: if set on the channel, the module will trigger `amd_on_machine`, `amd_on_human`, or `amd_on_notsure` automatically when AMD ends.

---

## Building as part of the FreeSWITCH source tree

1. Add as a git submodule

   ```bash
   git submodule add --name nexiscomms-mod_amd https://github.com/Nexis-Comms/mod_amd src/mod/applications/mod_amd
   ```
2. In `configure.ac`, add:

   ```
   AC_CONFIG_FILES([src/mod/applications/mod_amd/Makefile])
   ```
3. In `modules.conf`, add:

   ```
   applications/mod_amd
   ```
4. Build FreeSWITCH (entire project or just this module).

> Ensure the module is set to autoload (e.g., via your `modules.conf.xml`) or load it manually with `load mod_amd` in fs\_cli.


## Building mod_amd only (FreeSWITCH installed from official package repositories)

1. Install `libfreeswitch-dev`
    ```bash
    apt install -y libfreeswitch-dev
    ```

2. Copy Makefile.sample to Makefile
    ```bash
    cp Makefile.sample Makefile
    ```

3. Compile
    ```bash
    make && make install 
    ```

---

## Sample configuration

Create `conf/autoload_configs/amd.conf.xml`:

```xml
<configuration name="amd.conf" description="mod_amd Configuration">
  <settings>
    <param name="silence_threshold" value="256"/>
    <param name="maximum_word_length" value="5000"/>
    <param name="maximum_number_of_words" value="3"/>
    <param name="between_words_silence" value="50"/>
    <param name="min_word_length" value="100"/>
    <param name="total_analysis_time" value="5000"/>
    <param name="after_greeting_silence" value="800"/>
    <param name="greeting" value="1500"/>
    <param name="initial_silence" value="2500"/>
  </settings>
</configuration>
```

---

## Variables set by AMD

After AMD finishes, these channel variables are set:

* `amd_result` — one of:

  * `NOTSURE`: total\_analysis\_time elapsed without a clear decision
  * `HUMAN`: human detected
  * `MACHINE`: **machine** detected
* `amd_cause` — one of:

  * `INITIALSILENCE` (HUMAN)
  * `SILENCEAFTERGREETING` (HUMAN)
  * `MAXWORDLENGTH` (MACHINE)
  * `MAXWORDS` (MACHINE)
  * `LONGGREETING` (MACHINE)
  * `TOOLONG` (NOTSURE)
* `amd_result_epoch` — UNIX epoch when result was produced

### Execute-on hooks (optional)

If set on the channel, these are executed when AMD ends:

* `amd_on_human`
* `amd_on_machine`
* `amd_on_notsure`

Example (set before running AMD):

```
<action application="set" data="amd_on_human=playback(/path/you-are-human.wav)"/>
```

---

## Events

On decision, the module fires a **custom event** with subclass `amd`:

* `AMD-Result`: `HUMAN` | `MACHINE` | `NOTSURE`
* `AMD-Cause`: cause string listed above

You can also receive a queued copy of this event on the session.

---

## Usage

### 1) Dialplan application

Basic:

```xml
<extension name="amd_ext" continue="false">
  <condition field="destination_number" expression="^5555$">
    <action application="answer"/>
    <action application="amd"/>
    <action application="playback" data="/usr/local/freeswitch/sounds/en/us/callie/voicemail/8000/vm-hello.wav"/>
    <action application="info"/>
    <action application="hangup"/>
  </condition>
</extension>
```

With inline overrides (semicolon or comma separated):

```xml
<action application="amd" data="initial_silence=2000;greeting=1200;total_analysis_time=6000"/>
```

Originate to hit that extension:

```bash
originate {origination_caller_id_number=808111222,ignore_early_media=true,originate_timeout=45}sofia/gateway/mygateway/0044888888888 5555
```

### 2) CLI / Event Socket API

Start AMD by UUID (optionally override params):

**fs\_cli**

```
fs_cli> uuid_amd_detect 420f9e5a-3cf2-4e5d-b883-7c6f2b067a3a
+OK AMD detection started
```

With parameters:

```
fs_cli> uuid_amd_detect 420f9e5a-3cf2-4e5d-b883-7c6f2b067a3a initial_silence=2000;greeting=1200
```

**ESL (event socket)**

```
api uuid_amd_detect 420f9e5a-3cf2-4e5d-b883-7c6f2b067a3a total_analysis_time=7000
```

**Return codes**

* `+OK AMD detection started`
* `-ERR Usage: uuid_amd_detect <uuid> [key=val;...]`
* `-ERR No such channel <uuid>`
* `-ERR Channel not ready (no media)`
* `-ERR Failed to start AMD`

> **Note:** The channel must have **media up** (read codec and RTP) for AMD to attach its media bug.

---

## Parameter reference (overrides)

All parameters can be overridden in dialplan `amd` or via `uuid_amd_detect`:

* `initial_silence` (ms)
* `greeting` (ms)
* `after_greeting_silence` (ms)
* `total_analysis_time` (ms)
* `min_word_length` (ms)
* `between_words_silence` (ms)
* `maximum_number_of_words`
* `maximum_word_length` (ms)
* `silence_threshold` (amplitude score)

---

## Notes / Troubleshooting

* If you see `-ERR Channel not ready (no media)`, ensure the call is answered and RTP flowing.
* AMD is **non-blocking**: your call flow continues while detection runs.
* Check `amd_result`, `amd_cause`, and the `amd` event (or your execute-on hooks) to act on the outcome.

---

Happy detecting!
