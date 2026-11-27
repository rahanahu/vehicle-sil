import { mount } from 'svelte';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import Runtime from './Runtime.svelte';

mount(Runtime, { target: document.getElementById('app')! });
window.__vsilSmoke!.ready = true;
