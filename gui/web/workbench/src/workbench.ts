import {
  CUSTOM_ELEMENTS_SCHEMA,
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  computed,
  effect,
  inject,
  linkedSignal,
  signal,
} from '@angular/core';
import { DecimalPipe } from '@angular/common';
import {
  ActionError,
  connect,
  takeToken,
  watch,
  type CheckResult,
  type Front,
  type FrontClock,
  type FrontControls,
  type FrontScenario,
  type FrontStatus,
  type Intent,
  type ScenarioFile,
  type SessionView,
  type VariableInfo,
  type VariableRef,
  type VariablesState,
} from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/timeline';
import { parseDocument } from 'yaml';
import {
  addNetwork,
  open,
  removeNetwork,
  text,
  view,
  wire,
  type ChannelView,
  type DesignerModel,
} from '../../src/app/design/designer-model';
import { IoRow } from './io-row';

type FolderPicker = Window & {
  showDirectoryPicker?(options: { mode: 'readwrite' }): Promise<Folder>;
};
type Folder = FileSystemDirectoryHandle & {
  values(): AsyncIterable<FileSystemHandle>;
};
type Selection = VariableRef & { type: string; unit: string };
type Point = { sampleMs: number; value: number | boolean | null };
type WaveEntry = { key: string; trace: Selection | null; revision: number; actor: string };
type WaveMessage =
  | { kind: 'request'; instance: string; session: number | null; requester: string }
  | { kind: 'snapshot'; instance: string; target: string; entries: WaveEntry[] }
  | { kind: 'selection'; instance: string; entries: WaveEntry[] }
  | { kind: 'history'; instance: string; session: number; histories: [string, Point[]][] };
const locationQuery = new URLSearchParams(location.search);
// Retain the URL token in memory as well: storage can be unavailable or not copied to a new tab.
const pageToken = takeToken(window);
const waveStorage = 'vehicle-sil-workbench-waves';
const waveStorageKey = (instance: string) => `${waveStorage}:${instance}`;
const waveStorageHead = `${waveStorage}:last-instance`;
const pinStorage = 'vehicle-sil-workbench-pins';
function savedArray<T>(text: string | null, valid: (item: unknown) => item is T): T[] {
  try {
    const parsed: unknown = JSON.parse(text ?? '[]');
    return Array.isArray(parsed) ? parsed.filter(valid) : [];
  } catch {
    return [];
  }
}
const isPin = (item: unknown): item is string => typeof item === 'string';
const isTrace = (item: unknown): item is Selection => {
  if (!item || typeof item !== 'object') return false;
  const trace = item as Record<string, unknown>;
  return ['peer', 'name', 'type', 'unit'].every((key) => typeof trace[key] === 'string');
};
function readStored<T>(key: string, valid: (item: unknown) => item is T): T[] {
  try {
    return savedArray(localStorage.getItem(key), valid);
  } catch {
    return [];
  }
}
function saveStored(key: string, value: unknown): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // Site data can be disabled: the current tab remains usable without persistence.
  }
}
function restoreTraces(instance: string): Selection[] {
  let traces: Selection[] = [];
  let create = false;
  let previous: string | null = null;
  try {
    let saved = localStorage.getItem(waveStorageKey(instance));
    previous = localStorage.getItem(waveStorageHead);
    if (saved === null) {
      create = true;
      saved = previous ? localStorage.getItem(waveStorageKey(previous)) : null;
      saved ??= localStorage.getItem(waveStorage); // Before instance-scoped storage.
    }
    traces = savedArray(saved, isTrace);
  } catch {
    // Site data may be unavailable, while the current tab and run still work.
  }
  if (create) saveStored(waveStorageKey(instance), traces);
  try {
    // An already initialized, older instance may be observed late. It must not move the head
    // back from a newer one. Only the first initialization (or missing head) claims it.
    if ((create || previous === null) && localStorage.getItem(waveStorageHead) === previous)
      localStorage.setItem(waveStorageHead, instance);
  } catch {
    // Persistence is optional.
  }
  return traces;
}
const refKey = ({ peer, name }: VariableRef) => JSON.stringify([peer, name]);
const tabNames = [
  'ECU I/O',
  'Waveforms',
  'Scenario',
  'Traffic',
  'Logs',
  'Results',
  'Network Designer',
  'Settings',
] as const;
type Tab = (typeof tabNames)[number];

@Component({
  selector: 'app-workbench',
  imports: [DecimalPipe, IoRow],
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  templateUrl: './workbench.html',
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class Workbench {
  readonly tabs = tabNames;
  readonly fixedPeer = locationQuery.get('ecu');
  readonly detachedWave = locationQuery.get('view') === 'wave';
  readonly tab = signal<Tab>(this.detachedWave ? 'Waveforms' : 'ECU I/O');
  readonly front = signal<Front | null>(null);
  readonly status = signal<FrontStatus | null>(null);
  readonly clock = signal<FrontClock | null>(null);
  readonly controls = signal<FrontControls | null>(null);
  readonly runScenario = signal<FrontScenario | null>(null);
  readonly session = signal<SessionView | null>(null);
  readonly sessionReady = signal(false);
  readonly variables = signal<VariablesState | null>(null);
  readonly canRecord = signal(false);
  readonly files = signal<string[]>([]);
  readonly scenarios = signal<ScenarioFile[]>([]);
  readonly selectedFile = signal('');
  readonly selectedScenario = signal('');
  readonly speed = signal('1');
  readonly stopAt = signal('');
  readonly validStopAt = computed(() => {
    const stop = this.stopAt();
    return stop === '' || (/^\d+$/.test(stop) && Number(stop) >= 1 && Number(stop) <= 1e12);
  });
  readonly startPaused = signal(false);
  readonly pending = signal(false);
  readonly recordNext = signal(false);
  readonly recordedRun = signal<number | null>(null);
  readonly notice = signal('');
  readonly check = signal<CheckResult | null>(null);
  readonly configText = signal('');
  readonly savedConfigText = signal('');
  readonly configStamp = signal<string | null>(null);
  readonly configAnswer = signal<{ text: string; result: CheckResult } | null>(null);
  readonly configBusy = signal(false);
  readonly configDirty = computed(() => this.configText() !== this.savedConfigText());
  readonly configCheck = computed(() => this.configAnswer()?.result ?? null);
  readonly design = computed(() => {
    const answer = this.configAnswer();
    return answer ? view(open(answer.text), answer.result) : null;
  });
  readonly designCurrent = computed(
    () => this.configAnswer()?.text === this.configText() && !this.configBusy(),
  );
  readonly folder = signal<Folder | null>(null);
  readonly fallbackFolder = signal<{ name: string; files: File[] } | null>(null);
  readonly folderFiles = signal<string[]>([]);
  readonly yaml = signal('');
  readonly yamlName = signal('');
  readonly logQuery = signal('');
  readonly ecuQuery = signal('');
  readonly pinnedOnly = signal(false);
  readonly pins = signal<string[]>(readStored(pinStorage, isPin));
  readonly traces = signal<Selection[]>([]);
  readonly liveHistories = signal<Point[][]>([]);
  readonly inheritedHistories = signal(new Map<string, Point[]>());
  readonly waveTarget = computed(() => {
    // SessionFeed updates its authenticated target before notifying subscribers.
    this.session();
    return this.front()?.low.session.currentTarget() ?? null;
  });
  readonly authenticatedInstance = computed(() => {
    // Readiness changes after hello even when the server has no session.
    this.sessionReady();
    this.session();
    return this.front()?.low.session.currentInstance() ?? null;
  });
  readonly waveSession = computed(() => this.waveTarget()?.session ?? null);
  readonly histories = computed(() =>
    this.traces().map((trace, i) => {
      const newest = this.liveHistories()[i] ?? [];
      const previous = this.inheritedHistories().get(refKey(trace)) ?? [];
      return [
        ...previous.filter((point) => !newest.length || point.sampleMs < newest[0].sampleMs),
        ...newest,
      ].slice(-600);
    }),
  );
  readonly cursorA = signal<number | null>(null);
  readonly cursorB = signal<number | null>(null);
  readonly peerChoice = linkedSignal({
    source: () => ({
      peers: this.variables()?.peers.map((peer) => peer.name) ?? [],
      participants: this.session()?.check?.participants,
    }),
    computation: ({ peers, participants }, previous: { value: string } | undefined): string => {
      if (this.fixedPeer) return this.fixedPeer;
      const selected = previous?.value;
      if (
        selected &&
        (peers.includes(selected) ||
          !participants ||
          participants.some(
            (participant) => participant.name === selected && participant.enabled !== false,
          ))
      )
        return selected;
      return peers[0] ?? '';
    },
  });
  readonly peer = computed(() =>
    this.variables()?.peers.find((peer) => peer.name === this.peerChoice()),
  );
  readonly run = computed(() => this.status()?.run ?? 'none');
  readonly paused = computed(() => this.run() === 'paused');
  readonly live = computed(
    () =>
      this.pending() ||
      ['starting', 'waiting', 'running', 'paused', 'stopping'].includes(this.run()),
  );
  readonly statusText = computed(() => {
    const status = this.status();
    if (!status) return 'Connecting…';
    if (status.link !== 'online') return status.link;
    const states = {
      none: 'Ready',
      starting: 'Starting…',
      waiting: 'Waiting for participants…',
      running: 'Running',
      paused: 'Paused',
      stopping: 'Stopping…',
      ended: 'Run ended',
      crashed: 'Launcher exited',
    };
    return status.problem?.message ?? states[status.run];
  });
  readonly rows = computed(() => {
    const peer = this.peer();
    const query = this.ecuQuery().trim().toLowerCase();
    if (!peer) return [];
    return peer.variables
      .filter(
        (item) =>
          (!this.pinnedOnly() || this.isPinned(peer.name, item)) &&
          `${item.name} ${item.description}`.toLowerCase().includes(query),
      )
      .sort((a, b) => Number(this.isPinned(peer.name, b)) - Number(this.isPinned(peer.name, a)));
  });
  readonly groups = computed(() =>
    [true, false].map((writable) => {
      const rows = this.rows().filter((row) => row.writable === writable);
      const half = Math.ceil(rows.length / 2);
      return {
        key: refKey({ peer: this.peerChoice(), name: writable ? 'inputs' : 'outputs' }),
        title: writable ? 'Inputs' : 'Outputs',
        count: rows.length,
        lanes: [rows.slice(0, half), rows.slice(half)],
      };
    }),
  );
  readonly logs = computed(() => {
    const query = this.logQuery().trim().toLowerCase();
    return (this.session()?.logs ?? [])
      .filter((log) => `${log.source} ${log.line}`.toLowerCase().includes(query))
      .slice(-450);
  });
  readonly networks = computed(
    () => this.session()?.check?.networks ?? this.check()?.networks ?? [],
  );
  readonly result = computed(() => this.session()?.result);
  readonly canChooseFolder = typeof (window as FolderPicker).showDirectoryPicker === 'function';
  readonly late = computed(() =>
    this.clock()?.pacing !== 'not-paced' && this.clock()?.actual_speed != null
      ? (this.session()?.time?.lateMs ?? null)
      : null,
  );
  readonly waveDomain = computed(() => {
    const populated = this.histories().filter((points) => points.length);
    const start = populated.length ? Math.min(...populated.map((points) => points[0].sampleMs)) : 0;
    return {
      start,
      end: populated.length
        ? Math.max(start + 1, ...populated.map((points) => points.at(-1)!.sampleMs))
        : 1,
    };
  });
  readonly waveTracks = computed(() => {
    const domain = this.waveDomain();
    return this.traces().map((trace, index) => {
      const values = (this.histories()[index] ?? []).flatMap(({ sampleMs, value }) =>
        value == null || !Number.isFinite(Number(value))
          ? []
          : [{ sampleMs, value: Number(value) }],
      );
      const min =
        trace.type === 'bool' ? 0 : values.length ? Math.min(...values.map((p) => p.value)) : 0;
      const max =
        trace.type === 'bool' ? 1 : values.length ? Math.max(...values.map((p) => p.value)) : 1;
      let previousY: number | undefined;
      const path = values
        .map(({ sampleMs, value }) => {
          const x = this.waveX(sampleMs, domain);
          const y = 100 - (min === max ? 44 : ((value - min) / (max - min)) * 86);
          const command =
            previousY === undefined
              ? `M ${x} ${y}`
              : ['bool', 'enum'].includes(trace.type)
                ? `H ${x} V ${y}`
                : `L ${x} ${y}`;
          previousY = y;
          return command;
        })
        .join(' ');
      return { trace, path, min, max, id: refKey(trace) };
    });
  });

  private readonly destroy = inject(DestroyRef);
  private waveChannel: BroadcastChannel | null = null;
  private readonly waveActor = crypto.randomUUID();
  private waveRevision = 0;
  private waveInstanceSeen: string | null = null;
  private readonly waveEntries = new Map<string, WaveEntry>();
  private listRevision = 0;

  constructor() {
    if (this.fixedPeer) document.title = `${this.fixedPeer} · vehicle-sil`;
    if (this.detachedWave) document.title = 'Waveforms · vehicle-sil';
    const storage = (event: StorageEvent) => {
      const instance = this.authenticatedInstance() ?? this.waveInstanceSeen;
      if (instance && event.key === waveStorageKey(instance) && !this.waveChannel) {
        const traces = savedArray(event.newValue, isTrace);
        this.waveEntries.clear();
        for (const trace of traces) {
          const key = refKey(trace);
          this.waveEntries.set(key, { key, trace, revision: 0, actor: '' });
        }
        this.applyTraces(traces);
      }
      if (event.key === pinStorage) this.pins.set(savedArray(event.newValue, isPin));
    };
    window.addEventListener('storage', storage);
    this.destroy.onDestroy(() => window.removeEventListener('storage', storage));
    const channel =
      typeof BroadcastChannel === 'function' ? new BroadcastChannel(waveStorage) : null;
    this.waveChannel = channel;
    if (channel) {
      channel.onmessage = ({ data }: MessageEvent<WaveMessage>) => {
        const target = this.front()?.low.session.currentTarget();
        const instance = this.front()?.low.session.currentInstance() ?? this.waveInstanceSeen;
        if (!data || !instance || data.instance !== instance) return;
        this.ensureWaveInstance(instance);
        if (data.kind === 'request') {
          if (typeof data.requester === 'string' && this.waveEntries.size) {
            channel.postMessage({
              kind: 'snapshot',
              instance,
              target: data.requester,
              entries: [...this.waveEntries.values()],
            } satisfies WaveMessage);
          }
          if (target && data.session === target.session) {
            const histories = this.traces().map((trace, i): [string, Point[]] => [
              refKey(trace),
              this.histories()[i],
            ]);
            channel.postMessage({
              kind: 'history',
              instance,
              session: target.session,
              histories,
            } satisfies WaveMessage);
          }
        } else if (
          (data.kind === 'selection' ||
            (data.kind === 'snapshot' && data.target === this.waveActor)) &&
          Array.isArray(data.entries)
        ) {
          this.mergeWaveEntries(data.entries);
        } else if (data.kind === 'history' && target && data.session === target.session) {
          this.inheritedHistories.update((histories) => {
            const result = new Map(histories);
            for (const [key, points] of data.histories) {
              if (points.length > (result.get(key)?.length ?? 0))
                result.set(key, points.slice(-600));
            }
            return result;
          });
        }
      };
      this.destroy.onDestroy(() => channel.close());
    }
    effect(() => {
      const instance = this.authenticatedInstance();
      const session = this.waveSession();
      if (instance) this.ensureWaveInstance(instance);
      this.inheritedHistories.set(new Map());
      this.liveHistories.set([]);
      if (channel && instance)
        channel.postMessage({
          kind: 'request',
          instance,
          session,
          requester: this.waveActor,
        } satisfies WaveMessage);
    });
    effect((cleanup) => {
      const client = this.front();
      const traces = this.traces();
      if (!client || !traces.length) {
        this.liveHistories.set([]);
        return;
      }
      const feed = client.low.variables.watch(traces, { periodMs: 20, history: 600 });
      const release = watch(feed.values, () =>
        this.liveHistories.set(traces.map((trace) => feed.history(trace))),
      );
      cleanup(() => {
        release();
        feed.close();
      });
    });
    void this.action(async () => {
      const front = pageToken
        ? await connect({ base_url: new URL('/', location.href).href, token: pageToken })
        : await connect();
      this.front.set(front);
      this.destroy.onDestroy(() => front.close());
      const releases = [
        watch(front.status, (value) => this.status.set(value)),
        watch(front.clock, (value) => this.clock.set(value)),
        watch(front.controls, (value) => this.controls.set(value)),
        watch(front.scenario, (value) => this.runScenario.set(value)),
        watch(front.low.session.session, (value) => this.session.set(value)),
        watch(front.low.session.ready, (value) => this.sessionReady.set(value)),
        watch(front.low.variables.state, (value) => this.variables.set(value)),
        watch(front.low.app, (value) => this.canRecord.set(value?.variables?.available === true)),
      ];
      this.destroy.onDestroy(() => releases.forEach((release) => release()));
      const { files } = await front.low.configs();
      const initial = this.session()?.file ?? front.low.app.get()?.initial_file ?? '';
      const paths = files
        .filter(
          ({ simulation, path }) =>
            simulation === true || (simulation === undefined && path === initial),
        )
        .map(({ path }) => path);
      this.files.set(paths);
      await this.selectFile(paths.includes(initial) ? initial : (paths[0] ?? ''));
    });
  }

  async action(work: () => Promise<unknown>) {
    this.notice.set('');
    try {
      await work();
    } catch (error) {
      if (error instanceof Error && error.name === 'AbortError') return;
      this.notice.set(
        error instanceof ActionError
          ? [error.message, ...error.findings.map(({ message }) => message)].join('\n')
          : error instanceof Error
            ? error.message
            : String(error),
      );
    }
  }

  changeNetwork(select: HTMLSelectElement) {
    if (this.configDirty() && !window.confirm('Discard unsaved changes to this network?')) {
      select.value = this.selectedFile();
      return;
    }
    void this.action(async () => {
      await this.selectFile(select.value);
      if (this.tab() === 'Network Designer') await this.readConfig();
    });
  }

  showTab(tab: Tab) {
    this.tab.set(tab);
    if (tab === 'Network Designer' && this.selectedFile() && !this.configStamp()) {
      void this.action(() => this.readConfig());
    }
  }

  private async selectFile(file: string) {
    this.selectedFile.set(file);
    this.selectedScenario.set('');
    this.scenarios.set([]);
    this.configStamp.set(null);
    this.configText.set('');
    this.savedConfigText.set('');
    this.configAnswer.set(null);
    this.check.set(null);
    await this.refreshScenarios();
  }

  async refreshScenarios() {
    const revision = ++this.listRevision;
    const file = this.selectedFile();
    if (!file) return;
    const scenarios = (await this.front()!.scenarios({ file })).filter(
      (scenario) => !this.files().includes(scenario.path),
    );
    if (revision === this.listRevision) {
      this.scenarios.set(scenarios);
      if (!scenarios.some(({ path }) => path === this.selectedScenario()))
        this.selectedScenario.set('');
    }
  }

  intent(): Intent {
    const scenario = this.selectedScenario();
    const speed = this.speed();
    return {
      ...(scenario ? { scenario } : {}),
      ...(this.stopAt() ? { stop_at_ms: Number(this.stopAt()) } : {}),
      ...(speed ? { speed: speed === 'max' ? speed : Number(speed) } : {}),
      ...(!scenario && this.startPaused() ? { start_paused: true } : {}),
    };
  }

  async start(record: boolean) {
    const front = this.front()!;
    if (this.pending() || !this.controls()?.start?.ok) return;
    this.pending.set(true);
    this.recordNext.set(record);
    try {
      // The answer identifies this start even if session notifications are delayed or another
      // run starts before they arrive. RunControl adopts that same summary into the session feed.
      const intent = this.intent();
      const started = await front.low.run.start({
        file: this.selectedFile(),
        ...intent,
        disable: intent.disable ?? [],
      });
      if (started) {
        this.recordedRun.set(record ? started.id : null);
      } else if (record) {
        this.recordedRun.set(null);
        this.notice.set('The run started, but its session could not be identified for recording.');
      }
    } finally {
      this.pending.set(false);
      this.recordNext.set(false);
    }
  }

  async setSpeed(value: string) {
    if (this.live() && value)
      await this.front()!.time.setSpeed(value === 'max' ? value : Number(value));
    this.speed.set(value);
    this.check.set(null);
  }

  async assess() {
    const file = this.selectedFile();
    const scenario = this.selectedScenario();
    const stopAt = this.stopAt();
    const speed = this.speed();
    const paused = this.startPaused();
    const check = await this.front()!.check(this.intent(), { file, fresh: true });
    if (
      file === this.selectedFile() &&
      scenario === this.selectedScenario() &&
      stopAt === this.stopAt() &&
      speed === this.speed() &&
      paused === this.startPaused()
    ) {
      this.check.set(check);
    }
  }

  detach(view: 'ecu' | 'wave', ecu?: string) {
    const url = new URL(location.pathname, location.origin);
    url.searchParams.set('view', view);
    if (ecu) url.searchParams.set('ecu', ecu);
    // The new tab consumes and removes the fragment immediately, even when sessionStorage is blocked.
    if (pageToken) url.hash = `token=${encodeURIComponent(pageToken)}`;
    const tab = window.open('about:blank', '_blank');
    if (!tab) return this.notice.set('Allow pop-ups to open another view.');
    tab.opener = null;
    tab.location.replace(url.href);
  }

  isPinned(peer: string, item: VariableInfo) {
    return this.pins().includes(refKey({ peer, name: item.name }));
  }

  pin(peer: string, item: VariableInfo) {
    const key = refKey({ peer, name: item.name });
    this.pins.update((pins) =>
      pins.includes(key) ? pins.filter((pin) => pin !== key) : [...pins, key],
    );
    saveStored(pinStorage, this.pins());
  }

  addTrace(peer: string, item: VariableInfo) {
    const trace = { peer, name: item.name, type: item.type, unit: item.unit };
    if (this.traces().some((value) => refKey(value) === refKey(trace))) return;
    this.changeTrace(refKey(trace), trace);
  }

  removeTrace(trace: Selection) {
    this.changeTrace(refKey(trace), null);
  }

  private changeTrace(key: string, trace: Selection | null) {
    const instance = this.front()?.low.session.currentInstance() ?? this.waveInstanceSeen;
    if (!instance) return;
    this.ensureWaveInstance(instance);
    const entry = { key, trace, revision: ++this.waveRevision, actor: this.waveActor };
    this.mergeWaveEntries([entry]);
    if (this.waveChannel)
      this.waveChannel.postMessage({
        kind: 'selection',
        instance,
        entries: [entry],
      } satisfies WaveMessage);
  }

  private mergeWaveEntries(entries: WaveEntry[]) {
    let changed = false;
    for (const entry of entries) {
      if (
        !entry ||
        typeof entry.key !== 'string' ||
        typeof entry.actor !== 'string' ||
        !Number.isSafeInteger(entry.revision) ||
        entry.revision < 0 ||
        (entry.trace !== null && (!isTrace(entry.trace) || refKey(entry.trace) !== entry.key))
      )
        continue;
      this.waveRevision = Math.max(this.waveRevision, entry.revision);
      const old = this.waveEntries.get(entry.key);
      if (
        !old ||
        entry.revision > old.revision ||
        (entry.revision === old.revision && entry.actor > old.actor)
      ) {
        this.waveEntries.set(entry.key, entry);
        changed = true;
      }
    }
    if (changed) {
      const traces = [...this.waveEntries.values()]
        .filter((entry) => entry.trace !== null)
        .sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0))
        .map((entry) => entry.trace!);
      this.applyTraces(traces);
      if (this.waveInstanceSeen) saveStored(waveStorageKey(this.waveInstanceSeen), traces);
    }
  }

  private ensureWaveInstance(instance: string) {
    if (this.waveInstanceSeen === instance) return;
    this.waveInstanceSeen = instance;
    this.waveRevision = 0;
    this.waveEntries.clear();
    const traces = restoreTraces(instance);
    for (const trace of traces) {
      const key = refKey(trace);
      this.waveEntries.set(key, { key, trace, revision: 0, actor: '' });
    }
    this.applyTraces(traces);
  }

  private applyTraces(traces: Selection[]) {
    // A selection can arrive through both localStorage and BroadcastChannel. Avoid restarting
    // the history watch when they describe the same signals.
    if (JSON.stringify(this.traces()) !== JSON.stringify(traces)) this.traces.set(traces);
  }

  waveX(time: number, { start, end } = this.waveDomain()) {
    return ((time - start) * 1000) / (end - start);
  }

  async loadRecording() {
    const recording = await this.front()!.low.recording();
    if (!recording?.yaml || recording.session_id !== this.recordedRun()) {
      throw new Error(recording?.reason ?? 'No applied I/O writes were recorded in this run.');
    }
    this.yamlName.set(`run-${recording.session_id}.yaml`);
    this.yaml.set(recording.yaml);
    if (recording.omitted)
      this.notice.set(`${recording.omitted} writes were omitted from the recording.`);
  }

  async chooseFolder() {
    const picker = window as FolderPicker;
    if (!picker.showDirectoryPicker) return;
    const folder = await picker.showDirectoryPicker({ mode: 'readwrite' });
    this.folderFiles.set([]);
    this.fallbackFolder.set(null);
    this.folder.set(folder);
    await this.listFolder();
  }

  async chooseReadOnlyFolder(files: FileList | null) {
    const entries = Array.from(files ?? []);
    if (!entries.length) return;
    const name = entries[0].webkitRelativePath.split('/')[0];
    this.folderFiles.set([]);
    this.folder.set(null);
    this.fallbackFolder.set({
      name,
      files: entries.filter((file) => file.webkitRelativePath.split('/').length === 2),
    });
    await this.listFolder();
  }

  forgetFolder() {
    this.folder.set(null);
    this.fallbackFolder.set(null);
    this.folderFiles.set([]);
  }

  async listFolder() {
    const folder = this.folder();
    const fallback = this.fallbackFolder();
    const names: string[] = [];
    if (folder) {
      for await (const entry of folder.values()) {
        if (
          entry.kind === 'file' &&
          /\.ya?ml$/i.test(entry.name) &&
          (await this.isScenario(await (await folder.getFileHandle(entry.name)).getFile()))
        ) {
          names.push(entry.name);
        }
      }
    } else if (fallback) {
      for (const file of fallback.files) {
        if (/\.ya?ml$/i.test(file.name) && (await this.isScenario(file))) names.push(file.name);
      }
    }
    if (folder === this.folder() && fallback === this.fallbackFolder()) {
      this.folderFiles.set(names.sort((a, b) => a.localeCompare(b)));
    }
  }

  private async isScenario(file: File) {
    return parseDocument(await file.text()).has('vehicle_sil_scenario');
  }

  async openFolderFile(name: string) {
    if (!name) return;
    const folder = this.folder();
    const file = folder
      ? await (await folder.getFileHandle(name)).getFile()
      : this.fallbackFolder()?.files.find((entry) => entry.name === name);
    await this.openYaml(file ?? null);
  }

  async openYaml(file: File | null) {
    if (!file) return;
    if (!/\.ya?ml$/i.test(file.name)) throw new Error('Choose a YAML file.');
    const yaml = await file.text();
    if (!parseDocument(yaml).has('vehicle_sil_scenario'))
      throw new Error('This YAML file has no vehicle_sil_scenario key.');
    this.yaml.set(yaml);
    this.yamlName.set(file.name);
  }

  async saveYaml() {
    const folder = this.folder();
    const name = this.yamlName();
    const yaml = this.yaml();
    if (!/\.ya?ml$/i.test(name) || /[\\/]/.test(name))
      throw new Error('Enter a filename ending in .yaml or .yml.');
    if (!parseDocument(yaml).has('vehicle_sil_scenario'))
      throw new Error('A scenario YAML file needs the vehicle_sil_scenario key.');
    if (folder) {
      try {
        await folder.getFileHandle(name);
        if (!window.confirm(`Replace ${name} in ${folder.name}?`)) return;
      } catch (error) {
        if (!(error instanceof DOMException) || error.name !== 'NotFoundError') throw error;
      }
      const stream = await (await folder.getFileHandle(name, { create: true })).createWritable();
      try {
        await stream.write(yaml);
        await stream.close();
      } catch (error) {
        await stream.abort().catch(() => undefined);
        throw error;
      }
      await this.listFolder();
      await this.refreshScenarios();
    } else {
      const url = URL.createObjectURL(new Blob([yaml], { type: 'application/yaml' }));
      const a = document.createElement('a');
      a.href = url;
      a.download = name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    }
  }

  async readConfig() {
    const file = this.selectedFile();
    if (!file || this.configBusy()) return;
    if (this.configDirty() && !window.confirm('Reload and discard unsaved network changes?'))
      return;
    this.configBusy.set(true);
    try {
      const config = await this.front()!.readConfig({ file });
      this.configText.set(config.text);
      this.savedConfigText.set(config.text);
      this.configStamp.set(config.stamp);
      this.configAnswer.set(null);
    } finally {
      this.configBusy.set(false);
    }
    await this.checkConfig();
  }

  async checkConfig() {
    if (this.configBusy()) return;
    const file = this.selectedFile();
    const text = this.configText();
    this.configBusy.set(true);
    try {
      const result = await this.front()!.check({}, { file, text });
      this.configAnswer.set({ text, result });
    } finally {
      this.configBusy.set(false);
    }
  }

  async editNetwork(change: (model: DesignerModel) => void) {
    if (!this.designCurrent()) return;
    const model = open(this.configText());
    if (!model.editable) return;
    change(model);
    this.configText.set(text(model));
    await this.checkConfig();
  }

  async addBus(name: string, kind: string) {
    name = name.trim();
    if (!name) return;
    return this.editNetwork((model) => addNetwork(model, name, kind));
  }

  removeBus(name: string) {
    return this.editNetwork((model) => removeNetwork(model, name));
  }

  changeChannel(participant: string, channel: ChannelView, select: HTMLSelectElement) {
    const bus = select.value;
    select.value = channel.network;
    void this.action(() =>
      this.editNetwork((model) => wire(model, participant, channel.from, bus)),
    );
  }

  async refreshChannels() {
    this.configBusy.set(true);
    try {
      await this.front()!.describeAgain({ file: this.selectedFile() });
    } finally {
      this.configBusy.set(false);
    }
    await this.checkConfig();
  }

  async saveConfig() {
    const file = this.selectedFile();
    const text = this.configText();
    this.configBusy.set(true);
    try {
      const saved = await this.front()!.saveConfig({ file, text, stamp: this.configStamp() });
      this.configStamp.set(saved.stamp);
      this.savedConfigText.set(text);
      this.check.set(null);
    } finally {
      this.configBusy.set(false);
    }
  }
}
