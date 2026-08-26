---
title: Reference
nav_order: 6
has_children: true
---

# Reference

The precise documents. These are the working technical files from the repository, published here
unchanged apart from their front matter — so a link to `docs/API.md` on GitHub and the page you see
here are the same text.

| Page | What it is |
|---|---|
| [Settings reference](../manual.md) | Every setting in the interface, with the same explanation the **?** buttons show |
| [Hardware](hardware.md) | GPIO map, power, strip type, wiring notes |
| [API reference](../API.md) | Every REST endpoint, the WebSocket, MQTT topics, the Home Assistant entity list |
| [Home Assistant discovery notes](../HA-DISCOVERY.md) | Why the discovery payloads are shaped the way they are |
| [Architecture](../ARCHITECTURE.md) | Module ownership, the threading model, the LED priority ladder, the JSON contracts |
| [UI developer notes](../UI.md) | How the web interface is built, and how to run it against the mock server |
| [v2 code review](../REVIEW.md) | The 44 findings in the v2 firmware that motivated the rework |
| [Changelog](../CHANGELOG.md) | Every behaviour change, and the v2 → v3 configuration key map |

If you are looking for the *user*-facing explanation of something, it is more likely to be in
[Using BLLED](../using) or the [Guides](../guides).

---

## Previewing this site locally

The site is Jekyll with the [just-the-docs](https://just-the-docs.com/) remote theme, built by
GitHub Pages from the `docs/` folder of the `v3-rework` branch. `docs/Gemfile` pins the same
`github-pages` gem set that GitHub runs, so a local preview matches the published site:

```bash
cd docs
bundle install
bundle exec jekyll serve      # http://127.0.0.1:4000/BLLEDController/
```

You need Ruby and Bundler for that; nothing else in this repository does.

Pages are plain Markdown with a short YAML front matter block — `title`, `nav_order`, and `parent`
for a page that belongs to a section. Links between pages are relative and carry the `.md`
extension (`[Getting started](../getting-started.md)`), which works both on the published site and
when reading the files on GitHub. Images are relative too: `screenshots/dashboard-1280.png` from a
top-level page, `../screenshots/…` from inside a section.
