# vehicle-sil Workbench

An example English-language GUI built with the GUI Client. The same GUI server and
Control API run the simulation. This screen uses the public GUI Client and can
run alongside the standard screen on a separate port. Each server has its own
run. Detached Workbench tabs share the run of the Workbench that opened them.

## Start

Build a launcher with Control Server support, and the Angular Workbench.

```sh
npm --prefix gui/web/workbench ci
npm --prefix gui/web/workbench run build
python3 scripts/run-gui.py --workbench --no-browser --launcher <launcher> <simulation.yaml>
```

Open the complete URL printed by the command. See the
[GUI guide](../../../docs/gui.md) for the launcher, Control Server and Python setup.

## Try a run

- Select a configuration, leave **Free run** selected, and select **Start** or
  **Record & start**. Select **Start paused** to hold before the first step.
- Use the inline controls on **ECU I/O**. Press Enter to send a typed number.
  A slider sends the value when the adjustment finishes. During a pause a
  write remains pending until an ECU step applies it. If the result is lost,
  the screen says the outcome is unknown; check the value before writing again.
  **1 step** requests the minimum step reported by the launcher.
- Focus or hover over a signal to pin it or add it to **Waveforms**. Open an
  ECU or the waveform view in another browser tab. A detached ECU stays on
  the participant you selected. Current waveform history is bounded, sampled
  through the GUI Client and transferred between open tabs when available.
  Waveform selections are restored after a server restart when browser storage
  and Web Locks are available. Without Web Locks the current screen still works,
  but it does not advance the saved selection for the next server restart.
  History belongs to one server instance and run; it is not reused after a restart.
- Stop a recorded run, open **Scenario** and select **Load recording** to
  review or save the YAML. Recording exports the existing server format:
  applied writes and their virtual times, with the observed run length in its
  comments. Replay ends at the last step in the saved YAML.
- Choose a local folder in **Settings** when the browser supports directory
  access. **Scenario** can open and save YAML in that folder. A normal file
  picker and download are also available. To execute a file, put it in the
  server's `scenarios/` directory next to the selected configuration and
  refresh the Run menu.

## Views

**Scenario** shows check findings and the existing execution timeline after
a scenario run. **Traffic** shows network summaries and external connection
counts when the launcher reports them. **Logs** shows the GUI Server's log
feed. **Results** shows the run verdict and step results. **Configuration**
loads, checks and edits YAML through the server's existing save rules.

The pre-run graphical scenario timeline, frame-level CAN capture, network
diagram editor and direct execution from an arbitrary local scenario folder
need additional integration.
