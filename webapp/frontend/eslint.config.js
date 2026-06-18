import js from '@eslint/js';
import globals from 'globals';
import reactHooks from 'eslint-plugin-react-hooks';
import reactRefresh from 'eslint-plugin-react-refresh';
import tseslint from 'typescript-eslint';

// Flat ESLint config for the Vite + React + TS frontend (TDS-109).
// Tuned to be useful-but-not-noisy on a codebase that hadn't been linted:
// genuine bugs (rules-of-hooks, no-undef, unreachable code) are errors;
// stylistic / evolving-schema concerns (explicit any, unused-after-edit) are
// warnings so `npm run lint` stays actionable without a wall of failures.
export default tseslint.config(
  { ignores: ['dist', 'node_modules', '*.config.js', '*.config.ts'] },
  {
    files: ['**/*.{ts,tsx}'],
    extends: [js.configs.recommended, ...tseslint.configs.recommended],
    languageOptions: {
      ecmaVersion: 2022,
      globals: { ...globals.browser },
    },
    plugins: {
      'react-hooks': reactHooks,
      'react-refresh': reactRefresh,
    },
    rules: {
      ...reactHooks.configs.recommended.rules,
      'react-refresh/only-export-components': ['warn', { allowConstantExport: true }],
      // The config blobs from the SDK evolve, so `any` is used deliberately
      // in a few typed boundaries; flag it as a nudge, not a failure.
      '@typescript-eslint/no-explicit-any': 'warn',
      '@typescript-eslint/no-unused-vars': ['warn', { argsIgnorePattern: '^_', varsIgnorePattern: '^_' }],
      'react-hooks/exhaustive-deps': 'warn',
    },
  },
  {
    // Test files use vitest globals via explicit imports; nothing extra needed,
    // but keep them out of the react-refresh component rule.
    files: ['**/*.test.{ts,tsx}'],
    rules: { 'react-refresh/only-export-components': 'off' },
  },
);
