# Checks and findings (`vehicle_sil_launch check`)

[日本語](ja/simulation-check.md)

This is the reference for `vehicle_sil_launch check <file>`: the order in which it checks the configuration file against the ECU executables, the list of finding codes, and the output formats (a human-readable table and JSON). `vehicle_sil_launch run` runs the same check first.

| To find | See |
|---|---|
| What is checked, and in which order | [Check order](#check-order) |
| What each finding code means and how to fix it | [Finding codes](#finding-codes) |
| The severity when J1939 addresses overlap | [Overlapping J1939 addresses](#overlapping-j1939-addresses) |
| How to read the human-readable output | [Human-readable](#human-readable) |
| The keys of `check --json` | [JSON (`--json`)](#json---json) |
| Arguments and exit codes | "Arguments and exit codes" in [simulation-config.md](simulation-config.md#arguments-and-exit-codes) |

## Checks

`check` reports everything it finds, all at once, as findings. A finding has one of 3 severities.

| Severity | Start of the line | Result of the check |
|---|---|---|
| Error | `error:` | Fails the check. |
| Warning | `warning:` | Fails the check only with `--strict`. |
| Note | `note:` | Never fails the check. It tells you what the check could not confirm. |

### Check order

| Stage | What is checked | Details |
|---|---|---|
| 1 | The text of the configuration file | The format version, unknown and missing keys, value types and ranges, names, and the arguments and braces of `command`. |
| 2 | The executables | That each one exists, is not a directory, is not a batch file and (on Linux) has execute permission. |
| 3 | The `--describe` output | Starts each `describe: true` executable with `--describe --name <participant name>` and reads the JSON on its stdout. |
| 4 | The configuration file against the `--describe` output | Checks the two against each other. |

With `--connect`, after stage 3 the check also looks for the bridge of each connected network and runs its `--describe` with the arguments that the launcher gives it (see "Connecting a network outside (`external`, `--connect`)" in [simulation-config.md](simulation-config.md#connecting-a-network-outside-external---connect)).

**Notes on stage 1**

- The form of every variant (`variants`) is checked, including the variants that are not selected.
- Then the differences of the variant selected with `--variant` are applied on top of the common part, and from here on the check works on the result (see "Variants (`variants`, `--variant`)" in [simulation-config.md](simulation-config.md#variants-variants---variant)).
- When a variant is selected, the first line of the human-readable output ends with `(variant <name>)`.
- A participant with an error does not go on to stage 2.
- The errors meant here are a missing or invalid name, an invalid `command`, a duplicate name, a `describe` that is not a boolean, and errors in the arguments of `command`.
- Unknown keys and errors in `step_ms` do not stop the check.

**Notes on stage 2**

- Disabled participants (`enabled: false`, `--disable`) are not checked from stage 2 on.

**Notes on stage 3**

| Item | Rule |
|---|---|
| Start order | One executable at a time, in turn (not in parallel). |
| Working directory and stdin | The working directory is the directory of the configuration file. Stdin is empty. |
| Time limit | The executable is stopped after `--describe-timeout-ms` (default 10000 ms), even if it is still writing output. |
| What makes it an error for that participant | A nonzero exit code, not ending in time, output that is not JSON, or a different format version. |
| Ended by a signal or an exception | The finding message says so. |
| Attached text | An error comes with the last 10 lines of stderr. |
| How much is read | The first 1 MiB of stdout and the last 1 MiB of stderr. |
| Stdout cut off | The text attached to an error about the `--describe` output says so. |
| Stderr cut off | The attached stderr tail says so. |

### Finding codes

Each rule has a fixed name (the code; `code` in JSON) that the output shows. A severity marked "(unreadable)" is an error that makes the configuration file unreadable and gives exit code 3. The finding message itself says how to fix the problem; use this table to look up what a code means. The finding codes of scenarios (`--scenario`) are in [scenario.md](scenario.md), except for the one in the table below.

| Code | Severity | Meaning | Fix |
|---|---|---|---|
| `syntax` | Error (unreadable) | A YAML or JSON syntax error. | Fix the syntax at the place given in the attached text. |
| `not-a-configuration` | Error (unreadable) | The file is empty or is not a mapping. | Write a mapping with `vehicle_sil_simulation: 1`, `networks` and `participants`. |
| `encoding` | Error (unreadable) | The file is not UTF-8 (UTF-16, or it contains NUL). | Save the file as UTF-8. |
| `unsupported-yaml` | Error (unreadable for anchors and aliases) | The file has anchors and aliases, or a second document (`---`). | Write the values out in full, and delete the second and later documents or move them to other files. |
| `format-version` | Error (unreadable for an unknown version) | `vehicle_sil_simulation` is not 1. | Write `vehicle_sil_simulation: 1`, or use a vehicle-sil that can read the format of that file. |
| `file` | Error (unreadable) | The file does not exist, is a directory, is not a regular file, is too large or cannot be opened. | Check the path and the file. |
| `missing-key` | Error | A required key is missing. | Add the key named in the finding message. |
| `unknown-key` | Error | There is an unknown key. | Correct the key name (the finding message gives a close name and the list of valid keys). |
| `duplicate-key` | Error | The same key appears twice. | Delete one of them. |
| `value` | Error | A value of the wrong type or out of range, an invalid name, a placeholder in the executable path, or a `time_step_ms` that does not divide `stop_at_ms`. | Correct the value as the finding message says. |
| `duplicate-name` | Error | A participant name or a network name appears twice. | Delete one of them, or rename it. |
| `reserved-name` | Error | The participant name is `SimulationController`. | Use another name. |
| `launcher-argument` | Error | The `command` of an ecu_main executable has an argument of `ecu_main` or a placeholder. | Delete it, or write `describe: false` if the executable is not built with `ecu_main`. |
| `unknown-placeholder` | Error | The `command` of a `describe: false` participant has an unknown `{word}`. | Correct it to `{registry_uri}` or `{name}`, or write `{{word}}` to pass it as a string. |
| `no-registry-uri` | Note | The `command` of a `describe: false` participant has no `{registry_uri}`. | If the executable needs the URI of the registry, write `"{registry_uri}"` as the value of that argument. |
| `registry-on-network` | Note | `registry_uri` is not a loopback address. | To run on this computer only, use `127.0.0.1`, or do not write `registry_uri`. |
| `listen-variable-ignored` | Note | `VEHICLE_SIL_LISTEN_ON_NETWORK` in the environment of `vehicle_sil_launch` itself differs from the value that `run` passes to the participants (it is not used). | Only if participants are to join from other computers, write an address in `registry_uri` that those computers can connect to. |
| `step-not-checked` | Note | A `describe: false` participant has `step_ms`. | Delete `step_ms`, or set `describe: true` if it is an ecu_main executable. |
| `executable` | Error | The executable does not exist (a name without a separator is not on `PATH` either), is a directory or a batch file, or has no execute permission. | Correct the path in `command`, or put the executable there. |
| `found-on-path` | Note | The executable was found on `PATH` (the note shows where). | Check that the location shown is the executable you meant. |
| `not-required-ecu` | Error | A `describe: true` participant that is not `role: bridge` has `required: false`. | Delete `required: false` (to run without that ECU, use `enabled: false` or `--disable`). `role: bridge` is for the bridges, which join on their own; an ECU built with `ecu_main` does not start with `required: false`, also with `role: bridge`. |
| `no-required-participant` | Error | Every participant that is not disabled has `required: false`, or all participants are disabled (the simulation has no participant to wait for). | Enable at least one required (`required: true`) participant. |
| `describe-start` | Error | The executable cannot be started. | Check that the file is an executable that runs on this computer. |
| `describe-timeout` | Error | `--describe` does not end in time. | Fix what the ECU waits for during construction (a connection, input), or increase `--describe-timeout-ms`. |
| `describe-failed` | Error | The exit code of `--describe` is not 0. Exit code 4 most likely means an executable that is not built with `ecu_main`; for a `role: bridge` participant, it means that the bridge refused its arguments. On Windows, the exit status `0xC0000135` or `0xC0000139` means that the executable could not start because a DLL it needs is missing or of another version. | Fix the cause shown in the attached stderr, or write `describe: false` if the executable is not built with `ecu_main`. For a bridge, fix its `command` (the launcher adds `--name`, so do not write `--participant` or `--name` there). For a missing DLL, put the DLL next to the executable or in a folder on `PATH`; a bridge that this computer does not use can be left out with `--disable <name>`. |
| `describe-output` | Error | The output of `--describe` is not the `--describe` JSON. | Keep other output off stdout, or write `describe: false` if the executable is not built with `ecu_main`. |
| `describe-version` | Error | The format version of the `--describe` output (`vehicle_sil_ecu`) cannot be read. | Update the side that the finding message names (the ECU or `vehicle_sil_launch`). |
| `describe-unknown-value` | Error | A key of the `--describe` output that takes fixed values (`kind`, `use`, `type`, `causality`) has a value that this `vehicle_sil_launch` does not know (an ECU from a newer vehicle-sil). | Use a `vehicle_sil_launch` from the same vehicle-sil as the ECU, or a newer one. |
| `name-mismatch` | Error | `name` in the `--describe` output differs from the participant name (`main()` does not pass `--name` to `ecu_main`). | Change the ECU's `main()` so that it passes all arguments to `ecu_main`. |
| `step-mismatch` | Error | `step_ms` differs from the step size in the `--describe` output (also for a real-time ECU, which has no step size). | Change `step_ms` to the value in the finding message, or delete it. |
| `unknown-network` | Error | An ECU uses a network that is not in `networks`. | If it is meant to be the same bus, write the `network_names` line that the finding message gives for that participant; if it is another bus, add it to `networks`. |
| `kind-mismatch` | Error | An ECU uses a network as a different kind (CAN, LIN, Ethernet) from the one in `networks`. For a network renamed with `network_names`, the finding message gives both names: `'VehicleCAN' ('J1939_CAN' in the executable)`. | Correct `kind` in `networks`, or the network on the ECU side. |
| `address-conflict` | Error | 2 or more J1939 nodes on the same network ask for the same address, and one of them cannot work. | Give each node its own address. The finding message gives the `j1939_nodes` entry to write, or says that this file cannot change them (the executables do not take `--j1939-node`, or their code puts 2 nodes at one address). |
| `address-shared` | Warning | Of the nodes that ask for the same address, the node with the lowest NAME gets it, and all the others move to other addresses. | To decide which node is at which address, give each node its own address (the finding message gives the `j1939_nodes` entry to write). |
| `name-conflict` | Error | 2 or more J1939 nodes on the same network have the same NAME. | Give each node its own NAME: change the identity number of one of them with `j1939_nodes` (the finding message gives the entry to write). |
| `lin-masters` | Error | A LIN network has 2 or more masters (also the same participant twice). | Keep one master (make the others slaves, or call `lin_master()` only once). |
| `lin-no-master` | Error (a note when there is a `describe: false` participant) | There are LIN slaves and no master. | Add a participant that acts as the master. |
| `ethernet-duplicate-mac` | Error | On an Ethernet network, 2 or more participants report the same MAC address in their `--describe` output. | Give each participant its own address (`eth::ChannelOptions::mac`, and the source address of the frames it sends). |
| `ethernet-mac-unknown` | Note | An Ethernet network has a participant that does not report a MAC address in its `--describe` output. | Pass the address to `ethernet()` (`eth::ChannelOptions::mac`), or confirm yourself that the addresses do not overlap. |
| `unused-network` | Warning (a note when there is a `describe: false` participant) | No participant uses the network. | Delete the network, or check the network names on the ECU side. |
| `single-participant` | Warning (a note when there is a `describe: false` participant) | The network has only one participant (nobody receives what it sends). | Add the other participants of the network, or delete the network. |
| `test-participant-talks-on-bridged-network` | Warning | A `role: test` participant with a J1939 node may be on the same network as a bridge. | Make the participant only receive on that network (create no J1939 node), or disable the bridge for that run. |
| `connect-network` | Error | `--connect` names a network that is not in `networks`. | Correct the name (the finding message gives a close one). A network whose name has `=` cannot be connected: rename it. |
| `connect-kind` | Error | `--connect` names a LIN network (there is no bridge for LIN). | Leave that `--connect` out. |
| `connect-target` | Error | The target of `--connect` is empty, longer than 256 bytes, not UTF-8, has a control character or starts with `-`, or, for a CAN network, is not `virtual:<n>` or `physical:<n>` (`<n>` from 1). | Give a target that the bridge lists (`--list-interfaces`, `--list-channels`), or `none`. |
| `connect-name-taken` | Error | A participant of the file has the name of the bridge of a connected network (`bridge.<network>`). | Rename the participant. |
| `connect-filter` | Error | A connected Ethernet network has neither `external.ethertypes` nor `external.all_frames`, and the launcher cannot list the MAC addresses of the ECUs on it: one reports none, a participant without `role: test` has `describe: false`, or no ECU reports one (participants with `role: test` do not count). | Write `external.ethertypes` or `external.all_frames: true` for the network (also to pass the frames of participants with `role: test`), or give each ECU on it its MAC address. |
| `connect-tool-missing` | Error | The bridge of a connected network is neither next to `vehicle_sil_launch` nor in an absolute directory of `PATH`. | Build the bridge, and put it next to `vehicle_sil_launch` or its directory on `PATH`. |
| `connect-tool-failed` | Error | The bridge's `--describe` with the arguments of `--connect` failed (an exit code, the time limit, output that is not a description), or the bridge described another name or network. | Correct what the bridge's line in the finding message says (the target, `external`), or use the bridge of the vehicle-sil of `vehicle_sil_launch`. |
| `connect-loop` | Error | An enabled `role: bridge` participant reports a connected network in its `--describe` output: frames would go round between the two bridges. | Leave one of them out (`--disable <name>`, or leave the `--connect` out). |
| `connect-maybe-loop` | Warning | An enabled `role: bridge` participant has `describe: false`, and a network is connected (it may carry that network). | If it carries the network, leave one of the two bridges out. |
| `connect-not-real-time` | Warning | A network is connected, and the speed is not 1 (also without a speed: as fast as possible). `check` judges the file's `speed`; `run` the speed it runs at (`--speed`, or else the file's). | Run at speed 1: `speed: 1` in the file, or `run --speed 1`. |
| `connect-start-paused` | Warning | `run` only: a network is connected, and `--start-paused` is given. The devices on the bus go on while the simulation stands still. | To stop the simulation and look at it, run without `--connect`. |
| `not-checked` | Note | A `describe: false` participant (not checked). | No action needed. |
| `participant-disabled` | Note | A disabled participant (`enabled: false`, `--disable`; neither checked nor started). | No action needed. |
| `participant-manual` | Note | A participant that you start yourself (`manual: true`, `--manual`; `run` does not start it, but prints the command and waits). | Start it with the command that `run` prints. |
| `version-mismatch` | Note | The vehicle-sil version that the executable was built with differs from the version of `vehicle_sil_launch`. | If they do not work together, rebuild with the same version. |
| `coarse-time-step` | Warning | The file has `speed` (a number) and `time_step_ms`, and an ECU's steps do not start on time step boundaries. | Set `time_step_ms` to the value given in the finding message. |
| `control-link-not-offered` | Note | An ECU with variables does not report in its `--describe` output that it exposes them to the simulation (`--no-control`, or an old executable without `control`). | To read and write the variables, remove `--no-control`, or rebuild the ECU with a newer vehicle-sil. |
| `control-link-version` | Warning | An ECU with variables reports another message format version than `vehicle_sil_launch`, or another transport than `silkit`. The simulation runs, but `run` cannot read or write that ECU's variables. The message says which side is older. | If the ECU is older, rebuild it with the vehicle-sil of `vehicle_sil_launch`. If it is newer, use a `vehicle_sil_launch` of its vehicle-sil version or a newer one. For another transport, make the executable offer its variables over `silkit`. |
| `variable-map-unknown` | Error | The `from` variable of a mapping in `variables` does not exist in the executable. | Correct the `from` name (the finding message gives a close name or the list of the executable's variables). |
| `variable-map-type` | Error | The form of the mapping does not fit the type of the variable. | Write `scale`, `offset` for integer or real variables, and `map` for integer or enum variables. |
| `variable-map-value` | Error | The executable's variable cannot take a value in `map` (not an item of the enum, or out of range). | Change the values in `map` to values the variable can take. |
| `variable-map-collision` | Error | The name of a mapping equals the visible name of another variable of the executable, or the same variable has 2 mappings. | Rename the mapping, or delete one of the 2 mappings. |
| `variable-map-absent` | Error | The executable has a variable whose name is marked `absent: true`. | Delete that `absent: true`. |
| `variable-map-not-checked` | Note | A `describe: false` participant has `variables` (they cannot be compared before the run). | No action needed. |
| `network-map-undeclared` | Error | A key of `network_names` is not in `networks` (the key is the simulation's name, the value the executable's name). | Add the network to `networks`, or correct the key. If the value is in `networks`, the finding message gives the line the other way round (see [Matching network names](simulation-config.md#matching-network-names-network_names)). |
| `network-map-collision` | Error | `network_names` gives one network of the executable 2 names (one value under 2 keys), or after the mapping 2 networks of the executable have the same name (the executable itself also has a network with the name mapped to). | Keep one name for each network of the executable. To move a network to another bus in a variant, add the line that the finding message gives (`VehicleCAN: VehicleCAN`) to that variant. |
| `network-map-unknown` | Error | The executable does not use a network with the name of a value in `network_names`. | Correct the value (the finding message lists the executable's networks and gives a close name). |
| `network-map-unsupported` | Error | The executable of a participant with `network_names` cannot take `--map-network` (`features` in its `--describe` output has no `map-network`: an executable built with an earlier vehicle-sil). | Rebuild the executable, or give the network in `networks` the executable's name and write the mappings for the other participants. |
| `network-map-not-ascii` | Error | Windows launcher only: a key or value in `network_names` has a non-ASCII character (an ECU executable on Windows refuses such a `--map-network`). | Use ASCII names for the networks you map. |
| `network-map-not-checked` | Note | A `describe: false` participant has `network_names` (it cannot be checked). | No action needed for an ecu_main executable. |
| `j1939-node-undeclared` | Error | `network` of a `j1939_nodes` entry is not in `networks`, is not a CAN network, or is the executable's name of a network that `network_names` renames. | Write the simulation's name of the node's CAN network (the finding message gives a close name, or the name that `network_names` gives it). |
| `j1939-node-duplicate` | Error | 2 entries of `j1939_nodes` for the same node (the same `network` and `from`), or an entry without `from` beside another entry for the same network. Entries that change nothing count too. | Write the settings of one node in one entry; with more than one entry for a network, give each a `from`. |
| `j1939-node-not-ascii` | Error | Windows launcher only: `network` of a `j1939_nodes` entry has a non-ASCII character (an ECU executable on Windows refuses such a `--j1939-node`). | Use ASCII names for networks with `j1939_nodes`. |
| `j1939-node-not-checked` | Note | A `describe: false` participant has `j1939_nodes` that change something (they cannot be checked). | No action needed for an ecu_main executable. |
| `j1939-node-unsupported` | Error | The executable of a participant with `j1939_nodes` cannot take `--j1939-node` (`features` in its `--describe` output has no `j1939-node`: an executable built with an earlier vehicle-sil). | Rebuild the executable with this vehicle-sil or a later one. |
| `j1939-node-unknown` | Error | An entry of `j1939_nodes` takes no node: the executable has no node at the address of `from`, no node on that network, or no J1939 network of that name. | Correct `from` (the address in the executable's code, which the finding message lists) or `network`, or remove the entry. |
| `j1939-node-ambiguous` | Error | An entry without `from`, and the executable has nodes at different addresses on that network. | Add `from` with the address in the executable's code (the finding message lists them). |
| `j1939-node-indistinct` | Error | The executable's code puts 2 or more nodes at the address that the entry picks by (with `from`: that address; without: the only address of every node). The configuration file cannot tell them apart. | Remove the entry, and ask the ECU's author to give the nodes addresses of their own, or to take them as arguments. |
| `j1939-node-no-name` | Error | An entry sets a NAME field (`identity_number`, `function_instance`, `ecu_instance`) of a node without a NAME. | Remove those settings: only `address` can be changed for that node. |
| `j1939-node-any-address` | Error | An entry sets `address: 254` (any free address) for a node whose NAME is not arbitrary address capable, or for a node without a NAME. | Write an address of 0..253. |
| `j1939-node-changed` | Note | The entries of a participant's `j1939_nodes` change the address or NAME of its nodes (one note for each participant). | Check that no ECU relies on the old values (see the notes below). |
| `variant-participant` | Error | `participants` of a variant (`variants`) has a name that is not in `participants` of the configuration file. | Correct the name, or, for a participant that exists only in that variant, write it in `participants` with `enabled: false`. |
| `scenario-nothing-checked` | Warning | With this configuration (variant), all checking steps (`expect`, `wait_until`) of the `--scenario` scenario are skipped. | If only the variable names differ, map them with `variables`; if the scenario is not meant for this variant, write its target variants in `only` of the scenario. |
| `usage` | Error | A usage error (only in the JSON with `--json`). | Correct the arguments as the usage on stderr says. |
| `internal` | Error | An unexpected failure of `vehicle_sil_launch` itself (only in the JSON with `--json`). | Read the reason on stderr. |

### Notes on individual codes

| Code | Notes |
|---|---|
| `unsupported-yaml` | When there is a second document, the first document is still read. |
| `registry-on-network` | The registry and the participants listen on the network, and anyone who can connect to this computer can join the simulation (see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses)). |
| `listen-variable-ignored` | For the value that `run` passes, see "Listen addresses" in [simulation-run.md](simulation-run.md#listen-addresses). |
| `describe-version` | The finding message gives the vehicle-sil version of the ECU (if its output has one), the version of `vehicle_sil_launch`, and which of the two to update. |
| `describe-unknown-value` | The finding message gives the key and the value, the values it knows, and the vehicle-sil versions of both sides. |
| `describe-unknown-value` | It is an error of that participant only. Checking of the other participants continues. |
| `address-conflict` | For the conditions of the severity, see "Overlapping J1939 addresses" below. |
| `address-conflict`, `address-shared`, `name-conflict` | The fixes of the findings on one network suggest different free addresses and identity numbers, so all of them can be written at once. When the node has an entry of `j1939_nodes` already (also one that changes nothing), the fix is to set the values in that entry and keep its other settings. |
| `test-participant-talks-on-bridged-network` | The `role: bridge` participants are treated as bridges. If there is none, the `describe: false` participants without `role` are. |
| `test-participant-talks-on-bridged-network` | The participant's transmissions and its address claim (Address Claimed) go out on the real bus. |
| `test-participant-talks-on-bridged-network` | The finding message says whether a bridge is definitely or only possibly on the network, and how to fix it (see "Test participants and bridges (`role`)" in [simulation-config.md](simulation-config.md#test-participants-and-bridges-role)). |
| `test-participant-talks-on-bridged-network` | A network that `--connect` connects has a bridge too. The finding message then says to leave the `--connect` out in runs with the participant. |
| `single-participant` | Not reported for a network that `--connect` connects: its bridge counts as a participant. A connected network without participants is still `unused-network`. |
| `connect-*` | Only with `--connect` (see "Connecting a network outside (`external`, `--connect`)" in [simulation-config.md](simulation-config.md#connecting-a-network-outside-external---connect)). They are errors and warnings of the check only: the check does not open an adapter or a channel. The check of `run` has them too, so a warning fails `run --strict` before anything starts; what `run` finds when the bridge runs is `connect-failed` and `connect-lost` (see "Connecting a bus outside (`--connect`)" in [simulation-run.md](simulation-run.md#connecting-a-bus-outside---connect)). |
| `connect-filter` | Not reported while a participant has no description (`failed`, `skipped`): fix its error first. A disabled participant, a `role: bridge` one and a `role: test` one do not count: the default filter passes the frames of the ECUs only. |
| `connect-tool-failed` | The finding message gives the command and the bridge's line (its last line that starts with `error:`, or else its last line). The check runs the bridge only when nothing else is wrong with the network's `--connect` and every enabled participant was described (without a description, the frames that pass are not known). An exit status of `0xC0000135` or `0xC0000139` (Windows did not start the bridge for a DLL) is said as such; for `vehicle_sil_vector_xl_bridge`, the message names `vxlapi64.dll`. |
| `coarse-time-step` | The same message as the run finding (see "Time steps and ECU steps" in [simulation-run.md](simulation-run.md#time-steps-and-ecu-steps)). |
| `control-link-not-offered`, `control-link-version` | `run` cannot read or write the variables of that ECU (see "Connection to ECU variables (`control_link`)" in [simulation-run.md](simulation-run.md#connection-to-ecu-variables-control_link)). |
| `variable-map-*` | For how to write a mapping, see "Matching variable names and values (`variables`)" in [simulation-config.md](simulation-config.md#matching-variable-names-and-values-variables). |
| `variable-map-not-checked` | A mapping that does not match becomes the run finding `variable-map-not-applied`. |
| `network-map-unknown` | Findings that this line causes are not reported: `unknown-network` for the network of the executable with a name close to the value (without a close name, one of the same kind as the key), and `unused-network`, `single-participant` and `lin-no-master` for the network of the key. Findings for other networks are reported. |
| `network-map-unsupported` | The other rules continue with the mapped names. |
| `j1939-node-ambiguous`, `j1939-node-indistinct` | The person who fixes them differs: `j1939-node-ambiguous` is fixed in the configuration file (add `from`), `j1939-node-indistinct` only in the ECU's code, because `from` cannot tell apart nodes at the same address. |
| `j1939-node-*` (the ones about an entry that takes no node) | The nodes of that participant on that network are left out of `address-conflict`, `address-shared` and `name-conflict` (the values the file means are not known); conflicts of the other participants there are reported as before. `j1939-node-unknown` is not reported for a network that the participant's `network_names` keeps away (`network-map-unknown` says it), nor when the executable's only J1939 network keeps its own name and that name is not in `networks` (`unknown-network` says to add the line of `network_names`). |
| `j1939-node-unsupported` | The other rules use the values that the entries would give (the check applies them itself), so the findings are those that remain after the executable is rebuilt. `j1939-node-changed` is not reported for the participant. |
| `j1939-node-changed` | Lists each node that changes, with the value in its code and the new one. The check cannot see who relies on the old values: another ECU that has the old address in its code (as a destination, or to tell senders apart), or that looks the ECU up by its whole old NAME, and constants in the ECU's own code (a filter, a value in the data it sends). Not reported for a participant whose entries all give the values in the code, nor for a disabled participant. A node that takes any free address in its code gets no sentence about its address. |
| `network-map-not-checked` | The finding message gives the arguments that `run` adds in that run. They follow from the configuration after the selected variant and `--disable` are applied, and a participant to which `run` adds no argument gets no note. `run` adds `--map-network`, so an executable that is not an ecu_main executable ends with a usage error when it starts. |
| `scenario-nothing-checked` | The scenario passes without checking anything (see "Variants and parameters" in [scenario.md](scenario.md#variants-and-parameters)). |
| `usage` | Also covers a configuration file with variants and no `--variant`, a variant name that does not exist, and `--param` with a name that is not in the scenario. |

**`unknown-network`**: If there are networks of the same kind that this participant does not use and other participants do, the finding message names them (whether or not they are in `networks`). If it is meant to be one bus, the message then gives the `network_names` line to write for that participant (`network_names: { J1939_CAN: VehicleCAN }`, in the direction "simulation name: executable name"; the executable stays as it is). If it is another bus, it says to add the network to `networks`. When none of the networks it names is in `networks`, it says first to choose one name and add it to `networks`. If there is no network to name, the message says first to add the network to `networks`. A network renamed with `network_names` is compared under its new name.

**`single-participant`**: `role: test` participants are not counted. So the finding is reported for a network with one ECU and a test participant (such as a spy that only receives), and for a network with only test participants. If another network of the same kind also has a single ECU, a different one, the finding message names that network. If it is meant to be one bus, the message says to remove one of the names from `networks` and gives the `network_names` line to write for that ECU.

**`ethernet-duplicate-mac`**: The finding message gives the participants and the address. In a setup that uses the same address on purpose (a standby and an active unit, for example), keep `eth::ChannelOptions::mac` on one participant and do not pass it on the others. Only the note `ethernet-mac-unknown` then remains. A channel without `mac` cannot use `Accept::Addressed`, however. In SIL Kit, the order of frames sent in the same step can change from run to run.

**`ethernet-mac-unknown`**: Reported when an ECU does not pass its address to `ethernet()`. The note tells you that the check cannot confirm that the addresses do not overlap. A `role: bridge` participant that reports no address (a bridge carries the frames of others) is left out of this note and of `ethernet-duplicate-mac`.

**`participant-manual`**: If `registry_uri` is not a loopback address, the text of this note tells you to start the participant with the environment variable `VEHICLE_SIL_LISTEN_ON_NETWORK=1` set. The variable is needed only when you start the participant on the same computer as the launcher, with a URI that has a loopback address. When you start it on another computer, do not set it (see "Starting a participant yourself (`manual`, `--manual`)" in [simulation-config.md](simulation-config.md#starting-a-participant-yourself-manual---manual)). The note text does not distinguish which computer you start it on.

**`unused-network`, `single-participant`, `lin-no-master`**: These are reported only when the `--describe` output of every participant has been read. While any participant could not be checked (`failed` or `skipped`), they are not reported; fix that participant's errors first. When there is a `describe: false` participant, they are notes, because that participant may use the network. The finding message says so too.

### Overlapping J1939 addresses

The address is the one that a node requested when it was added (`nodes` in [ecu-executable.md](ecu-executable.md#keys)), and the address and NAME are those after the participant's `j1939_nodes` are applied (see "Matching J1939 addresses and NAMEs (`j1939_nodes`)" in [simulation-config.md](simulation-config.md#matching-j1939-addresses-and-names-j1939_nodes)). In the finding message, a node whose values come from an entry has `(set by j1939_nodes, line 14)` after its name. 254 (take a free address from the arbitrary address range) does not count as an overlap. For nodes that ask for the same address, the check works out the outcome of address claiming in vehicle-sil's J1939 stack.

| Nodes that ask for the same address | What happens | Severity |
|---|---|---|
| A node without a NAME is among them | A node without a NAME does not take part in address claiming and keeps sending from that address | Error |
| 2 or more nodes share the lowest NAME (equal NAMEs) | An equal NAME loses the address claim, so none of them gets the address | Error (`name-conflict` also says that the address is the same) |
| All have a NAME, and a node other than the one with the lowest NAME cannot take an arbitrary address (the arbitrary address bit of its NAME is not set) | The node with the lowest NAME gets the address, and the node that cannot move loses its address (Cannot Claim) | Error |
| All have a NAME, and every node other than the one with the lowest NAME can take an arbitrary address | The node with the lowest NAME gets the address, and the others move to free addresses | Warning |

When `address-shared` says that the other nodes "move to other addresses", it assumes that those addresses are free. A node without a NAME does not take part in address claiming, so if such a node is in the arbitrary address range, a node that moved may overlap with it. The check does not decide where nodes move to, so it does not check this.

Overlapping addresses and NAMEs on a network that is not in `networks` are not checked (fix `unknown-network` first). Neither are networks whose kind could not be read.

### Participants on a network, and Ethernet addresses

A participant uses a network when both the name and the kind match (the same holds for the table and for `networks[].participants` in JSON).

The name is compared after the participant's `network_names` is applied (the simulation's name). Every rule about a network, such as overlapping addresses and the LIN master, works with this name.

A participant whose `network_names` has an error that the configuration file alone shows (`network-map-undeclared`, `network-map-collision` for one value under 2 keys, the error `value` for its form, `network-map-not-ascii`) is not checked (`skipped`), so that nothing is compared under a name you did not mean. No rule looks at a line of `network_names` whose key and value are the same.

How Ethernet MAC addresses are handled:

| Item | Rule |
|---|---|
| Value that is checked | The value that the ECU passed to `ethernet()` and reported in its `--describe` output (`networks[].mac` in [ecu-executable.md](ecu-executable.md#keys)). |
| Actual frames | The source address of the frames actually sent is not looked at. |
| Different networks | The same address on different networks does not count as an overlap. |
| Shown in the table | The address is written in the networks column (`ETH (ethernet 02:00:00:00:00:01)`, or `ETH (ethernet)` if not reported). |

## Output

`check` writes a human-readable table (the default) or JSON (`--json`) to stdout. The same configuration file and executables always give the same output.

| Item | Rule |
|---|---|
| Line endings | LF on every OS (never `\r\n`, not even on Windows). |
| Name of the configuration file | Written as given in the argument. |
| Paths that the check builds (an executable it found, for example) | Written with `/` as the separator, even on Windows. |

### Human-readable

The output has these 4 parts, from top to bottom:

| Part | Content |
|---|---|
| Participant table | The step size and networks reported by `--describe`. |
| Network table | The participants that use each network. With `--connect`, a 4th column `outside`: the target of each connected network, `-` for the others. |
| Findings | One per line: errors, then warnings, then notes, each group in line order. |
| Result line | The numbers of errors, warnings and notes. |

In the participant table, J1939 networks show the address that each node asked for (`any` is 254). A network renamed with `network_names` is written as `VehicleCAN <- J1939_CAN (j1939 0x00)`: the simulation's name, then the executable's name. A node whose address `j1939_nodes` changes is written as `0x01<-0x00` (no space around the arrow), in the same direction: the address in the simulation, then the one in the code. A change of the NAME alone does not show in the table (the note `j1939-node-changed` says it). The name of a participant whose `role` is not `ecu` is followed by `(role: test)` or similar. The columns of the table are aligned by character count, so full-width characters break the alignment.

A finding line starts with `error:`, `warning:` or `note:` and has the form `<file>:<line>: <what, with which value, and how to fix it> [<code>]`.

| Part of the line | Content |
|---|---|
| `<file>` | The file name as given in the argument. |
| `<line>` | For a finding without a line, only the file name is written. |
| `[<code>]` | The finding code (the same as in the table above and as `code` in JSON). |
| Text on the following lines, indented by 4 spaces | Attached text (the tail of stderr, the location of a syntax error and so on). |

Use the code to look up its description in this document. As GCC does with `[-W...]`, the code goes at the end of the line, so the line still starts with `error:`, `warning:` or `note:`.

<!-- vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

An example with errors: it runs two instances of the same ECU, and has a wrong step size and a misspelled key.

<!-- two-engines.yaml -->
```yaml
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
  - name: Body_LIN
    kind: lin
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 20
  - name: Engine2
    command: [engine/vehicle_sil_engine_ecu]
  - name: Dashboard
    command: [dashboard/vehicle_sil_dashboard_ecu]
    requried: true
```

<!-- vehicle_sil_launch check two-engines.yaml -->
```text
two-engines.yaml: 3 participants, 2 networks

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Engine2      10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Engine2, Dashboard
Body_LIN   lin   -

error: two-engines.yaml:4: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME 0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an equal NAME loses the address claim): each node needs its own NAME; to do it in this file, write under participant 'Engine2': j1939_nodes: [{ network: J1939_CAN, address: 0x01, identity_number: 2 }] [name-conflict]
error: two-engines.yaml:11: participant 'Engine': step_ms is 20, but the executable's step is 10 ms: write step_ms: 10, or remove step_ms [step-mismatch]
error: two-engines.yaml:16: participant 'Dashboard': unknown key 'requried': did you mean 'required'? (the keys here: name, command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes) [unknown-key]
warning: two-engines.yaml:6: network 'Body_LIN': no participant uses it: remove it, or check the network names in the ECUs [unused-network]

failed: 3 errors, 1 warning, 0 notes
```

`name-conflict` goes away with the `j1939_nodes` entry that it gives, written under `Engine2`. For the steps, see "Run two instances of the same ECU" in [Integrate and run several ECUs](howto/integrate-ecus.md#run-two-instances-of-the-same-ecu).

### JSON (`--json`)

The output is one JSON object. It follows the same rules as the `--describe` JSON: a fixed key order, every key written even without a value, an indent of 2 spaces, and LF line endings even on Windows. A missing value is `null` (never an empty string), and a missing array is an empty array.

The first key, `vehicle_sil_check`, is the format version. The version goes up when the meaning of a key or a `code` changes, or when one is removed. Keys, and values of `code`, `severity`, `result` and `participants[].status`, may be added without a new version. A reader handles what it does not know as follows:

| Unknown item | What the reader does |
|---|---|
| Version | Rejects the output. |
| Key | Ignores it. |
| `code` | Handles it by its `severity`. |
| `severity` | Handles it as `"error"`. |
| `result` | Handles it as not passed (looks at the exit code). |
| `status` | Handles the participant as not checked. |

With `--json`, `check` writes JSON even when it stops before the check.

| Case | Exit code | JSON |
|---|---|---|
| The configuration file cannot be read | 3 | `result` is `"unreadable"`, and `participants` and `networks` are empty. |
| Usage error | 4 | `result` is `"usage"`, and `findings` has an error with `code` `usage`. |

On a usage error, `file` is the name of the configuration file if the arguments name one, and `null` if they do not (no file, 2 or more files and so on).

Top-level keys:

| Key | Value |
|---|---|
| `vehicle_sil_check` | Format version (currently 1) |
| `vehicle_sil` | The vehicle-sil version of `vehicle_sil_launch` |
| `file` | The configuration file (as given in the argument) |
| `result` | `"passed"`, `"failed"`, `"unreadable"`, `"usage"` |
| `strict` | Whether `--strict` was given |
| `errors`, `warnings`, `notes` | The numbers of errors, warnings and notes |
| `variant` | The selected variant (`--variant`). Present only for a configuration file with variants (`variants`) |
| `variants` | Array of the variant names in the configuration file. Present only for a configuration file with variants |
| `params` | The common `params`, overridden by the `params` of the selected variant. Present only when the configuration file has `params` |
| `stop_at_ms`, `start_timeout_ms`, `stop_timeout_ms`, `registry_uri` | Values from the configuration file |
| `participants[]` | Array of participants (in the order of the configuration file) |
| `networks[]` | Array of networks (in the order of the configuration file) |
| `findings[]` | Array of findings (in the same order as the human-readable output) |

- If `--variant` is missing or names no variant (`result` is `"usage"`), `variant` is `null` and `variants` gives the names you can choose from.
- Each element of `params` has `name`, `value` (`true`/`false`, a number or a string) and `from` (`configuration` or `variant`).
- If the configuration file cannot be read, `stop_at_ms`, `start_timeout_ms`, `stop_timeout_ms` and `registry_uri` are `null`.
- `stop_at_ms` and `registry_uri` are `null` if not written; `start_timeout_ms` and `stop_timeout_ms` have their defaults if not written.

Keys of each element of `participants[]`:

| Key | Value |
|---|---|
| `name` | Participant name (`null` if it could not be read) |
| `line` | Line in the configuration file |
| `command` | Executable and arguments |
| `describe`, `required` | Values from the configuration file |
| `enabled` | Value after `--disable` and `--enable` are applied |
| `manual` | Value after `--manual` is applied |
| `role` | `"ecu"`, `"test"`, `"bridge"` |
| `step_ms` | Value from the configuration file (`null` if not written) |
| `status` | State of the check (table below) |
| `reported` | The `--describe` output (only when `status` is `described`; `null` otherwise) |
| `variables` | Array of the ECU's variables as the simulation sees them (`ecu.connected`, then in declaration order) |
| `variable_map` | Array of the mappings written in `variables` of the configuration file (in the order written). Present only for participants that have `variables` |
| `network_map` | Array of the lines written in `network_names` of the configuration file (in the order written, after the lines of the variant are merged). `null` for participants without `network_names` |
| `j1939_nodes` | Array of the entries written in `j1939_nodes` of the configuration file, with the node each one takes (in the order written, after the entries of the variant are merged). `null` for participants without `j1939_nodes` |
| `control` | How the ECU exposes its variables, as reported in its `--describe` output |

| `status` | Meaning |
|---|---|
| `"described"` | The `--describe` output was read |
| `"not_checked"` | `describe: false` |
| `"failed"` | The executable is missing, or `--describe` failed |
| `"skipped"` | An error in the configuration file stopped the check of this participant |
| `"disabled"` | Disabled (even if the entry has errors) |

**`reported`**: The keys are `vehicle_sil`, `name`, `step_ns` and `networks` (in the same form as the `--describe` JSON). The network names in it are the executable's names, before `network_names` is applied, and the addresses and NAMEs of the nodes are those in the executable's code, before `j1939_nodes` is applied. The variables are in `variables`, described next.

**`variables`**: The names, types, ranges and units after the variable map (`variables` in the configuration file) is applied. They are what scenarios and the Control API see. Without a variable map, they are as the ECU reported them. Each element has the same form as `variables[]` of the `--describe` JSON (`name`, `type`, `causality`, `unit`, `min`, `max`, `step`, `description`, `items`, `builtin`; see "JSON format" in [ecu-executable.md](ecu-executable.md#json-format)). Use it to see which ECU has which variables before you run.

| Value of `variables` | Meaning |
|---|---|
| Empty array | The ECU has no variables. |
| `null` | Unknown. |

`variables` is `null` for a participant without `--describe` output (`describe: false`), a disabled participant, a participant whose `--describe` output could not be read, and an ECU from an old vehicle-sil that does not write `variables`.

**`variable_map`**: Mappings that could not be read (the error `value` and so on) are not included. Keys of each element:

| Key | Value |
|---|---|
| `name` | Name used in the simulation |
| `line` | Line in the configuration file |
| `from` | Variable name in the executable (`null` for `absent`) |
| `absent` | Whether `absent: true` was written |
| `scale`, `offset` | Values of the linear form (both `null` if the mapping is not in the linear form) |
| `unit` | Unit (`null` if not written) |
| `map` | Array of `name` and `value` (`null` if there is none) |

If only one of `scale` and `offset` is written, the other has its default, 1 or 0.

**`network_map`**: Each element has `name` (the simulation's name), `line` (the line in the configuration file) and `from` (the executable's name). Lines whose key and value are the same are included. Lines that could not be read (the error `value`) are not.

**`j1939_nodes`**: Entries that could not be read (the error `value` and so on) are not included. Keys of each element:

| Key | Value |
|---|---|
| `network` | The simulation's name of the network |
| `line` | Line in the configuration file |
| `from`, `address`, `identity_number`, `function_instance`, `ecu_instance` | The values written, as numbers (`null` if not written) |
| `node` | The node that the entry takes: `coded_address` (its address in the executable's code, as `address` in `reported`), then `address`, `j1939_name` and `arbitrary_address_capable` after the entry (in the same form as the nodes in `reported`). `null` for an entry that takes no node, an entry that changes nothing, and a participant that was not described |

**`control`**: The keys are `transport` (currently `"silkit"`) and `wire_version` (the message format version). It is `null` for a participant that does not report `control` (`--no-control`, an old executable), and for a participant that was not checked, failed or is disabled.

Keys of each element of `networks[]` and `findings[]`:

| Key | Value |
|---|---|
| `networks[].name` | Name of the network |
| `networks[].kind` | Kind (`null` if it could not be read) |
| `networks[].line` | Line in the configuration file |
| `networks[].participants` | Names of the participants that use the network (in the order of the configuration file; counted under the names after `network_names` is applied) |
| `networks[].outside` | On a network that `--connect` connects, its bridge (below); `null` on the other networks (no bridge is looked for for them, so the output does not depend on the tools on the computer) |
| `networks[].outside.tool` | The bridge's executable (`vehicle_sil_ethernet_bridge`, `vehicle_sil_vector_xl_bridge`) |
| `networks[].outside.tool_path` | Where it was found (`null` if it was not) |
| `networks[].outside.target` | The target as given to `--connect` |
| `networks[].outside.participant` | The bridge's participant name (`bridge.<network>`) |
| `networks[].outside.command` | The bridge and the arguments that the launcher gives it (without `--describe` and `--name`). `null` when an error about connecting the network (`connect-*`) is reported, or a participant has no description (the frames that pass are not known) |
| `findings[].severity` | `"error"`, `"warning"`, `"note"` |
| `findings[].code` | Finding code |
| `findings[].file` | The configuration file (for a finding about the scenario of `--scenario`, the scenario file) |
| `findings[].line` | Line (`null` if there is none) |
| `findings[].participant`, `findings[].network` | The participant and network concerned (`null` if there is none) |
| `findings[].message` | Finding message (one line) |
| `findings[].detail` | Attached text (`null` if there is none) |

<!-- engine-alone.yaml -->
```yaml
vehicle_sil_simulation: 1
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 20
```

<!-- vehicle_sil_launch check engine-alone.yaml --json -->
```json
{
  "vehicle_sil_check": 1,
  "vehicle_sil": "<version>",
  "file": "engine-alone.yaml",
  "result": "failed",
  "strict": false,
  "errors": 1,
  "warnings": 1,
  "notes": 0,
  "stop_at_ms": null,
  "start_timeout_ms": 30000,
  "stop_timeout_ms": 10000,
  "registry_uri": null,
  "participants": [
    {
      "name": "Engine",
      "line": 6,
      "command": [
        "engine/vehicle_sil_engine_ecu"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": 20,
      "status": "described",
      "reported": {
        "vehicle_sil": "<version>",
        "name": "Engine",
        "step_ns": 10000000,
        "networks": [
          {
            "name": "J1939_CAN",
            "kind": "can",
            "use": "j1939",
            "nodes": [
              {
                "address": 0,
                "j1939_name": "0x00000000FE000001",
                "arbitrary_address_capable": false
              }
            ],
            "mac": null
          }
        ]
      },
      "variables": [
        {
          "name": "ecu.connected",
          "type": "bool",
          "causality": "input",
          "unit": "",
          "min": null,
          "max": null,
          "step": null,
          "description": "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on",
          "items": [],
          "builtin": true
        },
        {
          "name": "engine.throttle",
          "type": "real",
          "causality": "input",
          "unit": "%",
          "min": 0,
          "max": 100,
          "step": null,
          "description": "accelerator pedal position",
          "items": [],
          "builtin": false
        },
        {
          "name": "engine.speed",
          "type": "integer",
          "causality": "output",
          "unit": "rpm",
          "min": null,
          "max": null,
          "step": null,
          "description": "",
          "items": [],
          "builtin": false
        }
      ],
      "network_map": null,
      "j1939_nodes": null,
      "control": {
        "transport": "silkit",
        "wire_version": 2
      }
    }
  ],
  "networks": [
    {
      "name": "J1939_CAN",
      "kind": "can",
      "line": 3,
      "participants": [
        "Engine"
      ],
      "outside": null
    }
  ],
  "findings": [
    {
      "severity": "error",
      "code": "step-mismatch",
      "file": "engine-alone.yaml",
      "line": 8,
      "participant": "Engine",
      "network": null,
      "message": "participant 'Engine': step_ms is 20, but the executable's step is 10 ms: write step_ms: 10, or remove step_ms",
      "detail": null
    },
    {
      "severity": "warning",
      "code": "single-participant",
      "file": "engine-alone.yaml",
      "line": 3,
      "participant": null,
      "network": "J1939_CAN",
      "message": "network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it",
      "detail": null
    }
  ]
}
```

The CTest test `launch_cli.docs` compares the example configuration files and outputs in this document with the real executables, byte for byte except for the `vehicle_sil` version.

## See also

| To find | See |
|---|---|
| Keys of the configuration file, arguments and exit codes | [simulation-config.md](simulation-config.md) |
| The `--describe` JSON | [ecu-executable.md](ecu-executable.md) |
| Running, and run findings | [simulation-run.md](simulation-run.md) |
| Steps to find and fix findings | [Integrate and run several ECUs](howto/integrate-ecus.md) |
