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
  Waveform selections are restored after a server restart at the same origin
  when browser storage and Web Locks are available. Pass the same `--port`
  on each launch to keep the origin. Without Web Locks the current screen still
  works, but it does not advance the saved selection for the next server restart.
  History belongs to one server instance and run; it is not reused after a restart.
- Select **Load recording** on **Scenario** either while a recorded run is active or after stopping it to review or save the YAML. Mid-run recordings omit `end_ms` so replay stops at the last `set` step; recordings loaded after the run ends include the finalized top-level `end_ms` from the run's end virtual time, so unedited replay runs up to that end time without dummy steps.
- To run a scenario, select a YAML file from the top **Scenario** menu (from the server's `scenarios/` directory next to the selected configuration). When the launcher supports handover, **After scenario** chooses **Stop run** (default) or **Pause for manual control**. Selecting **Pause for manual control** disables and omits **Stop at (ms)**. While the scenario replays, **ECU I/O** inputs are read-only; if the scenario passes with handover, the same run pauses at the scenario end time (staying paused even at **Fastest**), **ECU I/O** unlocks, and you can write inputs and continue with **1 step**, **Resume**, or **Speed**. Failed, stopped, or incomplete runs never hand over.
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
