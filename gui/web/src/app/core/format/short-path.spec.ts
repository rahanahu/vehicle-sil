import { shortPath } from './short-path';

describe('shortPath', () => {
  it('writes the home directory as ~ and keeps a short path whole', () => {
    expect(shortPath('/home/alice/sil')).toBe('~/sil');
    expect(shortPath('/Users/alice/sil/configs')).toBe('~/sil/configs');
    expect(shortPath('/opt/sil')).toBe('/opt/sil');
  });

  it('leaves out the middle of a long path', () => {
    expect(shortPath('/home/alice/repos/vehicle-sil/gui/server/tests/configs')).toBe(
      '~/…/tests/configs',
    );
    expect(shortPath('/srv/projects/a/b/c/configs')).toBe('/srv/…/c/configs');
    expect(shortPath('C:\\Users\\alice\\work\\sil\\gui\\configs')).toBe('~\\…\\gui\\configs');
  });

  it('does not take a directory that only starts like a home directory', () => {
    expect(shortPath('/homes/x')).toBe('/homes/x');
  });
});
