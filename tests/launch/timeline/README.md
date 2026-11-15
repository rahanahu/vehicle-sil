# Timeline references

Each `<name>.yaml` here is a scenario, and `<name>.timeline.json` the timeline file
(`run --scenario --timeline`, [docs/simulation-run.md](../../../docs/simulation-run.md)) that the scenario's
engine writes for it when it runs against the model ECUs of `tests/launch/test_scenario.cpp`. The test
`launch.timeline_files_are_the_references` compares them, so a change of the engine or of the file's form
shows as a difference of these files.

- Not compared (they differ from build to build or with the controller's step): `vehicle_sil`, `file`,
  `scenario.file`, `scenario.over_at_ms`, `tick_ms`.
- Never edit a reference by hand. Write them again with

  ```sh
  VEHICLE_SIL_UPDATE_TIMELINE_REFERENCES=1 ctest --preset linux-gcc-debug -R '^launch\.timeline_files_are_the_references$'
  ```

  and look at the difference: it must be what the change means to change.
- The scenarios use `set`, `ramp` and the waves that are worked out with whole-number arithmetic
  (`triangle`, `square`, `points`). No `sine`, `chirp` or `lag`: their last bit may differ between C
  libraries, and a reference is compared byte for byte.
