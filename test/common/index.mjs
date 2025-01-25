import { createRequire } from 'module';

const require = createRequire(import.meta.url);
const common = require('./index.js');

const {
  allowGlobals,
  buildType,
  canCreateSymLink,
  checkoutEOL,
  childShouldThrowAndAbort,
  createZeroFilledFile,
  enoughTestMem,
  escapePOSIXShell,
  expectsError,
  expectWarning,
  getArrayBufferViews,
  getBufferSources,
  getTTYfd,
  hasCrypto,
  hasIntl,
  hasIPv6,
  hasMultiLocalhost,
  isAIX,
  isAlive,
  isDumbTerminal,
  isFreeBSD,
  isIBMi,
  isInsideDirWithUnusualChars,
  isLinux,
  isLinuxPPCBE,
  isMainThread,
  isOpenBSD,
  isMacOS,
  isSunOS,
  isWindows,
  localIPv6Hosts,
  mustCall,
  mustCallAtLeast,
  mustNotCall,
  mustNotMutateObjectDeep,
  mustSucceed,
  nodeProcessAborted,
  parseTestFlags,
  PIPE,
  platformTimeout,
  printSkipMessage,
  runWithInvalidFD,
  skip,
  skipIf32Bits,
  skipIfDumbTerminal,
  skipIfEslintMissing,
  skipIfInspectorDisabled,
  spawnPromisified,
} = common;

const getPort = () => common.PORT;

export {
  allowGlobals,
  buildType,
  canCreateSymLink,
  checkoutEOL,
  childShouldThrowAndAbort,
  createRequire,
  createZeroFilledFile,
  enoughTestMem,
  escapePOSIXShell,
  expectsError,
  expectWarning,
  getArrayBufferViews,
  getBufferSources,
  getPort,
  getTTYfd,
  hasCrypto,
  hasIntl,
  hasIPv6,
  hasMultiLocalhost,
  isAIX,
  isAlive,
  isDumbTerminal,
  isFreeBSD,
  isIBMi,
  isInsideDirWithUnusualChars,
  isLinux,
  isLinuxPPCBE,
  isMainThread,
  isOpenBSD,
  isMacOS,
  isSunOS,
  isWindows,
  localIPv6Hosts,
  mustCall,
  mustCallAtLeast,
  mustNotCall,
  mustNotMutateObjectDeep,
  mustSucceed,
  nodeProcessAborted,
  parseTestFlags,
  PIPE,
  platformTimeout,
  printSkipMessage,
  runWithInvalidFD,
  skip,
  skipIf32Bits,
  skipIfDumbTerminal,
  skipIfEslintMissing,
  skipIfInspectorDisabled,
  spawnPromisified,
};
