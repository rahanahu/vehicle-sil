// The timeline page (timeline.html): one <vehicle-sil-timeline> element (vehicle-sil-timeline.js), and what a page
// adds to it. The file comes in one of 3 ways, in this order:
//   1. embedded in the page: the file's JSON as the text of the element with id "timeline-data" (a script element
//      of type application/json, which timeline.html has empty). A tool that makes one HTML file puts it there,
//      with '<', '>' and '&' written as <, > and &;
//   2. ?src=<url> on the page's own origin (a server that serves the page and the file). Served by the GUI server
//      (vehicle-sil-gui), the request carries the server's token: "#token=" in the address (taken out of it at
//      once), else the one this tab keeps, as every screen of the server does;
//   3. a file dropped on the page or chosen with "Open a file" (works when the page is opened as a file).
// The language is ?lang= (en or ja), else the browser's, else English. Every text comes from messages.js.
(function () {
  'use strict';

  const T = window.VehicleSilTimeline;
  const $ = (id) => document.getElementById(id);

  if (!T || !T.MESSAGES || !customElements.get('vehicle-sil-timeline')) {
    // The one text not in messages.js: it says that the element's script is missing (in both languages).
    document.body.textContent =
      // eslint-disable-next-line no-restricted-syntax
      'vehicle-sil-timeline.js is missing: put it next to timeline.html (it comes with the page). / ' +
      'vehicle-sil-timeline.js がありません。timeline.html の隣に置いてください（ページと一緒に配っています）。';
    return;
  }

  const timeline = document.querySelector('vehicle-sil-timeline');
  const lang = T.pickLanguage(location.search, navigator.languages ?? [navigator.language]);
  const t = (key, args) => T.text(lang, key, args);

  /** A text of messages.js into an element: a string, or an array of strings and [tag, text] (never parsed). */
  function fill(el, value) {
    const parts = Array.isArray(value) ? value : [value];
    el.replaceChildren(
      ...parts.map((part) => {
        if (!Array.isArray(part)) return part;
        const e = document.createElement(part[0]);
        e.textContent = part[1];
        return e;
      }),
    );
  }

  function showError(message) {
    const box = $('page-error');
    box.textContent = message;
    box.hidden = false;
    $('load').hidden = false;
  }

  // The token of the GUI server that serves this page: "#token=" in the address, taken out of it at once and kept
  // in this tab under the GUI's key (takeToken of its client library), else the one the tab keeps. A tab that the
  // GUI's "Timeline" link opens starts with a copy of the GUI tab's sessionStorage.
  const TOKEN_KEY = 'vehicle-sil-gui-token';

  function takeToken() {
    let storage = null;
    try {
      storage = window.sessionStorage;
    } catch {
      // the browser blocks site data: the token is kept in memory only
    }
    const match = /(?:^#|&)token=([^&]*)/.exec(location.hash);
    if (match) {
      history.replaceState(history.state, '', location.pathname + location.search);
      let token;
      try {
        token = decodeURIComponent(match[1]) || null;
      } catch {
        token = null;
      }
      if (token) {
        try {
          storage?.setItem(TOKEN_KEY, token);
        } catch {
          // kept in memory only
        }
        return token;
      }
    }
    try {
      return storage?.getItem(TOKEN_KEY) ?? null;
    } catch {
      return null;
    }
  }

  function readFile(file) {
    file.text().then(
      (text) => {
        $('page-error').hidden = true;
        timeline.fileName = file.name;
        timeline.data = text;
      },
      (e) => showError(t('pageLoad.cannotRead', { name: file.name, error: e.message })),
    );
  }

  document.documentElement.lang = lang;
  document.title = t('page.title');
  for (const el of document.querySelectorAll('[data-text]')) fill(el, t(el.dataset.text));
  timeline.lang = lang;
  // The element's words for loading, as this page says them: it has ?src= and takes a file dropped on it.
  const words = {};
  for (const language of T.LANGUAGES) {
    words[language] = {};
    for (const key of ['fileProtocol', 'notAddress', 'otherOrigin', 'failed', 'unauthorized'])
      words[language][`load.${key}`] = T.MESSAGES[language][`pageLoad.${key}`];
  }
  timeline.messages = words;

  timeline.addEventListener('load', (e) => {
    $('load').hidden = true;
    document.title = t('page.titleOf', {
      name: e.detail.scenario ?? (e.detail.manual ? t('manual.title') : t('page.scenario')),
    });
  });
  timeline.addEventListener('error', () => {
    $('load').hidden = false;
    document.title = t('page.title');
  });

  $('open').addEventListener('click', () => $('file').click());
  $('file').addEventListener('change', (e) => {
    const file = e.target.files && e.target.files[0];
    if (file) readFile(file);
    e.target.value = '';
  });
  document.addEventListener('dragover', (e) => {
    e.preventDefault();
    document.body.classList.add('dragging');
  });
  document.addEventListener('dragleave', (e) => {
    if (e.relatedTarget === null) document.body.classList.remove('dragging');
  });
  document.addEventListener('drop', (e) => {
    e.preventDefault();
    document.body.classList.remove('dragging');
    const file = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (file) readFile(file);
  });

  const token = takeToken();
  if (token) timeline.token = token;
  const embedded = $('timeline-data');
  const src = new URLSearchParams(location.search).get('src');
  if (embedded && embedded.textContent.trim() !== '') {
    timeline.data = embedded.textContent;
  } else if (src) {
    timeline.src = src;
    // "#token=" added to the address after a 401 changes only the hash: load the file again with it.
    window.addEventListener('hashchange', () => {
      const again = /(?:^#|&)token=/.test(location.hash) ? takeToken() : null;
      if (again) timeline.token = again;
    });
  }
})();
