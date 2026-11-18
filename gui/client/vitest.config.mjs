// Settings of the unit tests of gui/client (`npm run test:client` in gui/web). The tests of the parts run in jsdom, which
// Vitest loads through Vite as a web page: the files of gui/client are outside gui/web, so Vite must be allowed to read them.
export default { server: { fs: { strict: false } } };
