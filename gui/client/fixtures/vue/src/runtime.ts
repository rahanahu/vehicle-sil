import { createApp } from 'vue';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import Runtime from './Runtime.vue';

createApp(Runtime).mount('#app');
window.__vsilSmoke!.ready = true;
