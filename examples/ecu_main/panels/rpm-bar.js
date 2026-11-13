// A panel of your own for the browser GUI's dashboard (docs/gui.md, "持ち込みのパネル"): a custom
// element that shows one number as a bar. simulation.dashboard.yaml loads this module and places
// the element. The GUI sets three properties before it puts the element on the page:
//   client     the GUI client library's client (gui/client/README.md)
//   variables  [{peer, name}, ...] from the item's variables:
//   options    the item's options: (here {max})
// Plain JavaScript, no imports: it uses only what it is given.
class RpmBar extends HTMLElement {
  #watch = null;
  #stop = null;

  connectedCallback() {
    const ref = this.variables?.[0];
    const max = Number(this.options?.max) > 0 ? Number(this.options.max) : 100;
    const meter = document.createElement("meter");
    meter.min = 0;
    meter.max = max;
    meter.style.inlineSize = "100%";
    meter.setAttribute(
      "aria-label",
      ref ? `${ref.peer} ${ref.name}` : "no variable",
    );
    const text = document.createElement("output");
    text.dataset.testid = "rpm-bar-value";
    text.textContent = ref ? "—" : "no variable: give one under variables:";
    this.replaceChildren(meter, text);
    if (!ref) return;
    // Watch the value (every 100 ms) while the element is on the page.
    this.#watch = this.client.variables.watch([ref], { periodMs: 100 });
    this.#stop = this.#watch.values.subscribe((values) => {
      const value = values[ref.peer]?.[ref.name]?.value;
      if (typeof value !== "number") return;
      meter.value = Math.min(value, max);
      text.textContent = `${value} / ${max}`;
    });
  }

  disconnectedCallback() {
    this.#stop?.();
    this.#watch?.close();
    this.#stop = this.#watch = null;
  }
}

customElements.define("rpm-bar", RpmBar);
