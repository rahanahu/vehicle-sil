import { HttpErrorResponse, HttpInterceptorFn } from '@angular/common/http';
import { inject } from '@angular/core';
import { tap } from 'rxjs';
import { AuthToken } from './auth-token';

/** API（`api/` で始まる相対の URL）にトークンを付ける。401 が返ったら、トークンが断られたことを知らせる。 */
export const authInterceptor: HttpInterceptorFn = (request, next) => {
  if (!request.url.startsWith('api/')) {
    return next(request);
  }
  const auth = inject(AuthToken);
  const token = auth.token();
  const authorized = token
    ? request.clone({ setHeaders: { Authorization: `Bearer ${token}` } })
    : request;
  return next(authorized).pipe(
    tap({
      error: (error: unknown) => {
        if (error instanceof HttpErrorResponse && error.status === 401) {
          auth.markRejected();
        }
      },
    }),
  );
};
