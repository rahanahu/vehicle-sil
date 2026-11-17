# Constraints (only an explicit decision of the user changes them; do not loosen them after an incident)

Rules that every agent follows before any work. Keep reasons and history out of this file.

## Do not open

- <paths or kinds of documents that agents must not read, for example licensed standards or third-party downloads under `.local/vendor/`>

## Publication (everything that goes into Git)

- <what must not be copied into tracked files>

## Assets

- <files that must not be deleted or overwritten, for example VM images and backups>
- Do not delete or discard a working tree that has uncommitted changes without asking the user.

## Wording for requests to sub-agents and to work in other working trees

"Before you start, read `<absolute path of this file>` and follow it. A working tree has no `.local/`."
