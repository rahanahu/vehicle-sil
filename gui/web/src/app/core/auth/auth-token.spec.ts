import { DOCUMENT } from '@angular/core';
import { HttpClient, provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { authInterceptor } from './auth-interceptor';
import { AuthToken } from './auth-token';

describe('AuthToken', () => {
  beforeEach(() => {
    sessionStorage.clear();
    history.replaceState(null, '', '/');
  });

  function create(): AuthToken {
    TestBed.configureTestingModule({});
    return TestBed.inject(AuthToken);
  }

  it('takes the token from the URL, removes it from the URL and keeps it for a reload', () => {
    history.replaceState(null, '', '/some/path?x=1#token=abc-_123');
    const auth = create();
    expect(auth.token()).toBe('abc-_123');
    const location = TestBed.inject(DOCUMENT).location;
    expect(location.hash).toBe('');
    expect(location.pathname + location.search).toBe('/some/path?x=1');
    expect(sessionStorage.getItem('vehicle-sil-gui-token')).toBe('abc-_123');
  });

  it('reads the kept token after a reload', () => {
    sessionStorage.setItem('vehicle-sil-gui-token', 'kept');
    expect(create().token()).toBe('kept');
  });

  it('has no token when the page was not opened from the server URL', () => {
    expect(create().token()).toBeNull();
  });
});

describe('authInterceptor', () => {
  let http: HttpTestingController;
  let client: HttpClient;
  let auth: AuthToken;

  beforeEach(() => {
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    TestBed.configureTestingModule({
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
      ],
    });
    http = TestBed.inject(HttpTestingController);
    client = TestBed.inject(HttpClient);
    auth = TestBed.inject(AuthToken);
  });

  afterEach(() => {
    http.verify();
    sessionStorage.clear();
  });

  it('sends the token to the API only', () => {
    client.get('api/app').subscribe();
    client.get('favicon.svg').subscribe();
    expect(http.expectOne('api/app').request.headers.get('Authorization')).toBe('Bearer secret');
    expect(http.expectOne('favicon.svg').request.headers.has('Authorization')).toBe(false);
  });

  it('notes that the server refused the token', () => {
    client.get('api/app').subscribe({ error: () => undefined });
    expect(auth.rejected()).toBe(false);
    http.expectOne('api/app').flush({ message: 'no' }, { status: 401, statusText: 'Unauthorized' });
    expect(auth.rejected()).toBe(true);
  });
});
