// Flat-config module for the emitted figure engine assets.
//
// Adopters whose ESLint configuration lints all JavaScript or TypeScript files with
// type-aware parsing (parserOptions.project: true) and without a root tsconfig that
// includes tools/figures/ can import this entry to lint the figure assets syntactically:
//
//   import figures from './tools/figures/eslint.figures.mjs';
//
// Pass your typescript-eslint plugin or config object (e.g. `figures(ts)` or `figures(tseslint)`).
// The returned flat-config entry disables type-aware parsing and type-checked rules for
// the emitted figure files while leaving every other lint rule as you configured it.

/**
 * Returns an ESLint flat-config entry for the engine-emitted figure assets.
 *
 * @param {object} [ts] The adopter's typescript-eslint object (e.g. `@typescript-eslint/eslint-plugin`).
 * @returns {object} Flat-config entry with project: false and disable-type-checked rules.
 */
export default function figures(ts) {
  const disableTypeChecked =
    (ts?.configs?.['disable-type-checked']?.rules) ??
    (ts?.configs?.['flat/disable-type-checked']?.rules) ??
    (ts?.configs?.disableTypeChecked?.rules) ??
    {};

  return {
    files: [
      'tools/figures/**/*.js',
      'tools/figures/**/*.mjs',
      'tools/figures/**/*.ts',
    ],
    languageOptions: {
      parserOptions: {
        project: false,
      },
    },
    rules: {
      ...disableTypeChecked,
    },
  };
}

export { figures };
