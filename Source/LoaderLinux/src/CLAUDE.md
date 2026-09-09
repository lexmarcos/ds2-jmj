# ds2os loader — frontend rules

This is the GUI of the Linux loader: a Tauri app whose only job is to pick a
server, confirm the game and its Proton prefix were found, and launch.

Stack: React 19, Vite, TypeScript 7, Tailwind v4 (CSS-first), Base UI.

UI copy is written in Brazilian Portuguese. Code, comments, commit messages and
this file are in English, matching the rest of the repository.

## The visual system

Everything below is defined once in `src/styles/theme.css` as `@theme` tokens
and reaches components as Tailwind utilities. **Never hardcode a colour, a font
or a radius in a component.** If a value is missing, add a token.

### Where the look comes from

Majula at dusk: a blue-black sky over an endless sea, and one bonfire. That
gives three rules that decide most questions:

1. **The ground is night, not off-black.** `--color-void` is `#0f1420` and has
   real blue in it. Never reach for `#000`, `#111` or a neutral grey.
2. **Text is parchment, not white.** `--color-bone` is `#ded6c6`. Pure white
   never appears in this interface.
3. **There is exactly one warm light.** `--color-ember` is an antique brass, and
   it marks the launch action and the selected server. If a second thing on
   screen glows, one of them is wrong.

### Palette

| Token | Use |
| --- | --- |
| `void` | Page ground |
| `panel` / `raised` | Surfaces; `raised` only on top of `panel` |
| `line` / `line-strong` | Hairlines; `line-strong` separates sections |
| `bone` / `bone-dim` / `bone-faint` | Primary, secondary, tertiary text |
| `ember` / `ember-bright` / `ember-deep` | The single warm accent |

### Phantom colours are semantic

`invader`, `coop` and `sentinel` come from the game's own summoning system and
keep those meanings here. Pick them for what a state *means*, never for how it
looks:

- **`invader`** (red) — destructive actions, errors, failed states
- **`coop`** (bone white) — healthy states, success, "found it"
- **`sentinel`** (blue) — information, neutral notices

Use `<Status tone="…">` rather than colouring text by hand; that component is
the only place these map to classes.

### Typography

Two families, each with a job. A third face exists only for data.

- **Spectral** (`font-serif`) — content: server names, descriptions, headings.
  This is the item-description voice.
- **Archivo** (`font-sans`) — interface: buttons, labels, counts. The default.
- **JetBrains Mono** (`.data` class) — filesystem paths, hostnames, ports,
  addresses. Real data only, **never labels or headings**.

Sizes come from the `--text-*` scale (minor third). Body copy stays under about
70 characters per line.

Fonts are bundled through `@fontsource`, never fetched from a CDN. This is a
desktop app and it must work offline.

### Things that are not allowed here

These read as generic and are explicitly out:

- ALL-CAPS labels or eyebrows above headings
- Accenting one word of a heading in a different colour or weight
- Meta strings joined with middle dots (`A · B · C`)
- A `→` appended to button or link text
- Rounding every element the same, or wrapping every block in an identical card
  with a soft grey shadow — structure here is carried by **hairlines** (`Rule`)
  and spacing, not by cards
- Decorative gradients, and numbered markers (`01 / 02`) on things that are not
  actually a sequence

### Motion

One orchestrated moment beats scattered effects. Transitions answer a user
action (opening, selecting, toggling) and last 150ms with `ease-out`. Nothing
animates on page load. `prefers-reduced-motion` is already honoured globally in
`theme.css`; do not add animations that bypass it.

## Components

**All UI primitives live in `src/components/`, are built on Base UI, and are
reused. Feature code never styles raw elements.**

If you are writing `className="border border-line bg-panel rounded-md …"` inside
`src/features/`, stop: that belongs in a component.

### The rule in practice

1. Need a control? Check `src/components/index.ts` first.
2. Not there? Build it in `src/components/`, on top of the matching Base UI
   part (`@base-ui/react/<component>`), and export it from the barrel.
3. Only then use it in a feature.

Features may compose components and apply **layout** utilities (`flex`, `gap`,
`px`, `truncate`). They may not apply colour, border, radius or typography
utilities — those are the component's business.

### Writing a component

- Props are explicit and typed; export the props interface.
- Wrap Base UI, do not reimplement it. Accessibility, focus management and
  keyboard behaviour come from Base UI for free, and hand-rolled substitutes
  lose them.
- Take `className` for layout only, and merge it with `cn()` from `@/lib/cn`.
- Style state with Base UI's `data-*` attributes (`data-selected`,
  `data-highlighted`) rather than tracking state in React to swap classes.
- Give the component one job. `Status` shows a state; it does not fetch one.

## Writing the interface

Words are design content. Keep them plain.

- A button says what happens: "Preparar lançamento", not "Enviar" or "OK". The
  same action keeps the same name everywhere.
- Errors say what went wrong and what to do next, in the interface's voice. They
  do not apologise and they are never vague.
- An empty screen is an invitation to act, so it carries the action that fills
  it (see `ServerList`).
- Sentence case everywhere. No filler.

## Software practices

- **TypeScript is strict**, including `noUncheckedIndexedAccess` and
  `exactOptionalPropertyTypes`. Do not reach for `any` or `as` to silence the
  compiler; if a type is wrong, fix the type. Note that TS 7 removed `baseUrl` —
  path aliases are declared in `paths` alone, and mirrored in `vite.config.ts`.
- **Backend failures are values, not exceptions.** Every Tauri call goes through
  `api` in `src/lib/api.ts` and returns `Result<T>`. The UI must render the
  failure; never let a rejected promise disappear.
- **Keep state where it is used.** Lift it only when a second component needs
  it. There is no global store and this app does not need one.
- **Never invent backend shapes.** Types in `src/lib/api.ts` mirror the Rust
  structs in `crates/ds2os-core`. Change them together, in the same commit.
- **The accessibility floor is not optional**: visible keyboard focus (already
  global), labelled controls, real `<button>` elements for actions, and states
  that do not rely on colour alone — that is why `Status` pairs a dot with text.

## Verifying

Run both before you claim anything works:

```bash
npm run typecheck   # tsc --noEmit
npm run build       # typecheck + vite build
```

The Rust side has its own checks:

```bash
cargo test -p ds2os-core
cargo run -q -p ds2os-core --example detect   # prints what it finds on this machine
```

A green typecheck says nothing about whether the design landed. Run the app and
look at it:

```bash
npm run dev &          # Tauri loads the dev server in debug builds
cargo run -p ds2os-loader
```

Subtle values do not survive a dark ground. When something is meant to be
visible and is not, prove which half is wrong before tuning: crank the value to
an opaque primary and re-run. If it appears, the value was too weak; if it does
not, the selector or the stacking order is.
