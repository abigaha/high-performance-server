type SessionAction = () => void;

export function createUnauthorizedHandler(
  clearSession: SessionAction,
  navigateToLogin: SessionAction,
): SessionAction {
  return () => {
    clearSession();
    navigateToLogin();
  };
}
