# Decisions to make before you start

[日本語](../ja/howto/design-choices.md)

This page lists what the ECU authors and the integrator (the person who puts the ECUs together in a configuration file) should decide before anyone starts writing ECUs, and how to choose when you are unsure. It has no procedures; each section links to the page that shows how.

These are recommendations, not rules. The [`check` command](integrate-ecus.md#step-3-check-the-configuration-and-read-the-findings) only checks whether the ECUs agree with each other on what you chose.

| Decision | Recommendation |
|---|---|
| [What goes into one ECU](#what-goes-into-one-ecu) | Make one executable per ECU of the real vehicle. |
| [What goes on the bus and what becomes a variable](#what-goes-on-the-bus-and-what-becomes-a-variable) | Values that ECUs exchange go on the bus. Values that you feed in from outside, and internal values that you want to see, become variables. |
| [Which variables to read for verification](#which-variables-to-read-for-verification) | Read the sender or the receiver, depending on what you want to verify. |
| [Network names](#network-names) | Pick one name per bus, and keep it as a constant in a shared header. |
| [Addresses and NAMEs](#addresses-and-names) | If an ECU may run as 2 or more instances, let it take its address and NAME as arguments. |
| [Variable names](#variable-names) | Use the form `<ECU>.<signal>`, and give each variable a unit and a range. |
| [Code shared between ECUs](#code-shared-between-ecus) | Share only the message numbers and how values are packed. |

## What goes into one ECU

Make one executable per ECU of the real vehicle. One ECU is then also one author, one build and one unit that you can swap.

- The integrator can disable ECUs one at a time, or replace one with another version (see "Run with an ECU disabled (`enabled`, `--disable`)" in [Integrate and run several ECUs](integrate-ecus.md#run-with-an-ecu-disabled-enabled---disable)).
- If an ECU that yours talks to does not exist yet, write a small ECU yourself that stands in for it, and run them together. The dashboard ECU of the example is such a stand-in: it plays the ECU that receives the engine speed from the engine ECU.

## What goes on the bus and what becomes a variable

Variables and the bus are separate things. A variable is a member variable of the ECU, exposed so that it can be read and written from outside the ECU; it never travels on the bus. A value goes on the bus only if your code sends it.

| Value | Where it goes | Example |
|---|---|---|
| A value that other ECUs use | The bus (a message) | Engine speed |
| A value that comes from a sensor or a switch in the real vehicle | An input variable | `engine.throttle` |
| Internal state that you want to check in tests | An output variable | A state number, a fault flag, an integrated value |
| A value sent on the bus that you also want to see at the sender | Both the bus and an output variable | `engine.speed` |

Avoid also declaring a value that arrives over the bus from another ECU as an input variable of the receiver. A write from outside would set the value without going through the bus, and the test would no longer show that the communication works. To see a received value, make it an output variable (`dashboard.engine_speed` in the example).

For how to write these, see [Create your first ECU](first-ecu.md).

## Which variables to read for verification

The same engine speed tells you different things depending on which ECU you read it from.

| To verify | Read | Example |
|---|---|---|
| That the ECU's model computes correctly | An output variable of that ECU | `engine.speed` |
| That the value reached the other ECU over the bus | An output variable of the receiving ECU | `dashboard.engine_speed` |
| That the receiver notices when the value stops arriving | The variable of the receiving ECU that signals message loss | `dashboard.engine_lost` |

If the sender and the receiver show different values, the cause is in the communication: a network name mismatch, a disconnected ECU, or a difference in how the values are packed. Write model tests against the sender's variables and communication tests against the receiver's. When a test fails, you then know which of the two is at fault.

## Network names

One name is one bus. ECUs that use the same name connect to the same bus. ECUs with different names do not see each other's frames.

- If there are 2 buses (for example, 2 CAN buses with different bit rates), decide on 2 names. An ECU on both buses opens 2 networks in its code.
- The name is a value in the ECU's code. A configuration file can map another name onto it (`network_names`), but every configuration file then needs that mapping. Decide on the names before work starts and share them with everyone (see "Step 1: Agree on names and share them" in [Integrate and run several ECUs](integrate-ecus.md#step-1-agree-on-names-and-share-them)).
- Do not scatter the name across an ECU's sources as string literals. Define it once, as a constant, in a header that the ECUs share. A change then touches one place, and the compiler catches typos.
- The bit rate is separate from the name. A normal run does not use it: a frame arrives in the simulation step after the one in which it was sent (a simulation step is the interval by which the simulation advances virtual time). Only 2 things use the bit rate: runs that include a CAN network simulator, and bridges to real hardware (see [CAN foundation](../internals/can.md) and [Vector XL CAN Bridge](../bridges.md)).

## Addresses and NAMEs

J1939 addresses and NAMEs are also values in the ECU's code.

- For an ECU that runs as a single instance, constants are fine.
- If an executable may run as 2 or more instances (one ECU per wheel or per axle, for example), let it take its address and NAME as arguments from the start. If you add this later, `check` reports `name-conflict` for the 2 instances, the configuration file cannot fix it, and the integrator has to wait for the rebuilt ECU (see "Run two instances of the same ECU" in [Integrate and run several ECUs](integrate-ecus.md#run-two-instances-of-the-same-ecu)).
- In examples, tests and documentation, use a Manufacturer Code that does not look like the code of a real company.

## Variable names

- Use the form `<ECU>.<signal>`, as in `engine.speed`. When the variables of several ECUs appear together on one screen or in one scenario, the name tells you which ECU each one belongs to.
- Give each variable a unit, and each input a range. Writes outside the range are rejected, and the unit appears on the screen and in command output.
- If different executables ended up with different names for the same thing, the variable map in the configuration file can align them (see "Matching variable names and values (`variables`)" in [simulation-config.md](../simulation-config.md#matching-variable-names-and-values-variables)). If you can align the names in the ECUs instead, that is simpler.

## Code shared between ECUs

Share between ECUs only the rules of the bus: message numbers, how values are packed, periods and network names. In the example, [examples/ecu_main/engine_speed.hpp](../../examples/ecu_main/engine_speed.hpp) is the header that holds the message number and how the value is packed. The example is small, so each ECU writes the network name as a string. In a real project, keep the name in the shared header, as "[Network names](#network-names)" recommends.

Do not share models or state. If you do, different people can no longer build the ECUs separately, each at its own version.

## What to read next

- Build one ECU: [Create your first ECU](first-ecu.md)
- Run the ECUs you received together: [Integrate and run several ECUs](integrate-ecus.md)
- Turn your checks into automated tests: [Write tests with scenarios](scenario-tests.md)
