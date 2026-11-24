import { clientErrorSink } from './client';
import type { Front, VariableView } from './front-types';
import { Writable, type Readable } from './values';
import type { VariableInfo, VariableRef } from './variables';
import type { WriteValue } from './variable-state';

const READBACK_MS = 500;

export interface VariableEditorState {
  readonly info:
    | (Omit<Readonly<VariableInfo>, 'enum'> & {
        readonly enum: readonly Readonly<{ name: string; value: number }>[] | null;
      })
    | undefined;
  readonly view: VariableView;
  readonly shown: WriteValue | null;
  readonly waiting: boolean;
  readonly canWrite: boolean;
}

export interface VariableEditor {
  readonly state: Readable<VariableEditorState>;
  /** Returns false when no edit can be attempted. Read the write result in state. */
  write(value: WriteValue): boolean;
  setVisible(visible: boolean): void;
  close(): void;
}

/** Binds a typed editor to a variable, including the acknowledgement and readback of a Boolean edit. */
export function createVariableEditor(front: Front, ref: VariableRef): VariableEditor {
  const client = front.low;
  const variable = front.variable(ref);
  const binding = client.variables.bind({ ...ref }, { visible: false });
  let requested: boolean | null = null;
  let awaiting: boolean | null = null;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let target = client.session.currentTarget();
  let closed = false;
  let stopReading: (() => void) | undefined;
  let infoSource: VariableInfo | undefined;
  let info: VariableEditorState['info'];
  const read = (bound = binding.state.get()): VariableEditorState => {
    const view = variable.get();
    if (bound.info !== infoSource) {
      infoSource = bound.info;
      info = infoSource && {
        ...infoSource,
        enum: infoSource.enum?.map((item) => ({ ...item })) ?? null,
      };
    }
    const waiting = bound.pending !== null || view.writing || awaiting !== null;
    const current = client.session.currentTarget();
    return {
      info,
      view,
      shown: bound.pending ?? view.value,
      waiting,
      canWrite:
        !closed &&
        client.session.ready.get() &&
        current !== null &&
        current.run_id === client.variables.state.get().session &&
        view.can_write &&
        bound.block === null &&
        !waiting,
    };
  };
  const state = new Writable(read(), Object.is, clientErrorSink(client));

  const reset = () => {
    clearTimeout(timer);
    timer = undefined;
    requested = null;
    awaiting = null;
  };
  const refresh = () => {
    if (closed) return;
    const current = client.session.currentTarget();
    const bound = binding.state.get();
    const shown = read(bound);
    if (closed) return;
    if (
      current?.instance !== target?.instance ||
      current?.run_id !== target?.run_id ||
      !shown.view.can_write
    ) {
      reset();
    } else if (awaiting !== null && shown.view.value === awaiting) {
      reset();
    } else if (requested !== null && bound.writeStatus.kind === 'applied') {
      if (shown.view.value !== requested) {
        awaiting = requested;
        timer = setTimeout(() => {
          reset();
          state.set(read());
        }, READBACK_MS);
      }
      requested = null;
    } else if ('message' in bound.writeStatus) {
      reset();
    }
    target = current;
    state.set(read());
  };
  const unsubscribe = binding.state.subscribe(refresh);
  const unsubscribeSession = client.session.session.subscribe(refresh);
  const unsubscribeReady = client.session.ready.subscribe(refresh);

  return {
    state: state.readonly(),
    write(value) {
      const { canWrite, info } = read();
      if (!canWrite || !info) return false;
      const valid =
        typeof value === 'boolean'
          ? info.type === 'bool'
          : typeof value === 'string'
            ? info.type === 'enum' && info.enum?.some((item) => item.name === value)
            : Number.isFinite(value) &&
              (info.type === 'real' ||
                (info.type === 'integer' && Number.isInteger(value)) ||
                (info.type === 'enum' && info.enum?.some((item) => item.value === value))) &&
              (info.min === null || value >= info.min) &&
              (info.max === null || value <= info.max);
      if (!valid) {
        binding.reject('the edit does not match the variable type or range');
        return false;
      }
      requested = typeof value === 'boolean' ? value : null;
      binding.write(value);
      return true;
    },
    setVisible(visible) {
      if (closed || visible === !!stopReading) return;
      if (visible) {
        stopReading = variable.subscribe(refresh);
      } else {
        const stop = stopReading;
        stopReading = undefined;
        stop?.();
      }
      refresh();
    },
    close() {
      if (closed) return;
      closed = true;
      unsubscribe();
      unsubscribeSession();
      unsubscribeReady();
      stopReading?.();
      reset();
      binding.close();
      state.set(read());
    },
  };
}
