<?php

/**
 * @file
 * Mollom client/server interaction functions.
 *
 * @todo
 * - Major: Decide on whether to use
 *   - abstract classes/methods (that inheriting classes have to implement in
 *     order to extend the base class), or
 *   - interfaces in combination with a strategy pattern (passing in two
 *     separate MollomClientInterface and MollomProtocolInterface compatible
 *     classes to the Mollom class constructor, which are then turned into
 *     $this->client and $this->protocol).
 *   It's unlikely that other clients will implement more than one protocol, so
 *   the double-inheritance as in this file/Drupal's case is not the usual case.
 *   - $serversInit override might be problematic for strategy pattern: Server
 *     URLs are different for REST and XMLRPC, and also different in (future)
 *     testing mode.
 * - Abstract MOLLOM_ERROR request response handling. REST and XMLRPC use
 *   different ways to express a (local) configuration error. Predefined errors
 *   are used in "upstream" methods and calling code.
 * - Doing a "verifyKey" with an empty server list leads to two/duplicate and
 *   unnecessary GET site/$id requests, one for the server list, subsequent ones
 *   for whatever/verifyKey. Some resources can be cached.
 */

/**
 * A catchable Mollom exception.
 *
 * The Mollom class internally uses exceptions to handle client-side server
 * balancing and eventually HTTP request errors within the Mollom::request()
 * method. All exceptions thrown in the Mollom class and derived classes should
 * be instances of the MollomException class if they pertain to errors that can
 * be catched/handled within the class. Other errors should not use the
 * MollomException class and handled differently.
 *
 * No MollomException is supposed to pile up as a user-facing fatal error. All
 * functions that invoke Mollom::request() have to catch Mollom exceptions.
 *
 * @see Mollom::query()
 * @see Mollom::request()
 *
 * @param $arguments
 *   (optional) A associative array containing additional information about the
 *   performed request that failed, typically:
 *   - data: (array) An associative array containing HTTP GET/POST/PUT request
 *     query parameters sent to the server.
 *   - response: (mixed) The server response, either as string, or the already
 *     parsed response, e.g. as an array.
 */
class MollomException extends Exception {
  public $arguments = array();

  function __construct($message = '', $code = 0, Exception $previous = NULL, array $arguments = array()) {
    // PHP <5.3 throws a fatal error when passing more arguments to Exception.
    if (version_compare(phpversion(), '5.3') >= 0) {
      parent::__construct($message, $code, $previous);
    }
    else {
      parent::__construct($message, $code);
    }
    $this->setArguments($arguments);
  }

  public function setArguments($arguments) {
    $this->arguments = $arguments;
    return $this;
  }

  public function getArguments() {
    return $this->arguments;
  }
}

/**
 * Mollom network error exception.
 *
 * Thrown in case a HTTP request results in code <= 0, denoting a low-level
 * communication error.
 *
 * @todo Still needed? Introduced as a debugging measure for
 *   MollomServerListRecoveryTestCase, which didn't end up in a
 *   NETWORK_ERROR when expected. But turned out to be a logic error in
 *   refreshServers() instead. Not actively caught anywhere yet.
 */
class MollomNetworkException extends MollomException {
}

/**
 * Mollom authentication error exception.
 *
 * Thrown in case API keys or other authentication parameters are invalid.
 */
class MollomAuthenticationException extends MollomException {
}

/**
 * Mollom server redirect exception.
 *
 * Thrown when a Mollom server asks the client to use next server in the server
 * list.
 */
class MollomRedirectException extends MollomException {
}

/**
 * Mollom server refresh exception.
 *
 * Thrown when a Mollom server asks the client to update the server list.
 */
class MollomRefreshException extends MollomException {
}

/**
 * The base class for Mollom client implementations.
 */
abstract class Mollom {
  /**
   * The public Mollom API key to use for request authentication.
   *
   * @var string
   */
  public $keyPublic = '';

  /**
   * The private Mollom API key to use for request authentication.
   *
   * @var string
   */
  public $keyPrivate = '';

  /**
   * The list of Mollom servers to communicate with, as returned by Mollom.
   */
  public $servers = array();

  /**
   * A hard-coded list of Mollom servers to fetch the server list from.
   */
  public $serversInit = array('http://xmlrpc1.mollom.com', 'http://xmlrpc2.mollom.com', 'http://xmlrpc3.mollom.com');

  /**
   * The status code of the last response or TRUE if it succeeded.
   */
  public $lastResponseCode = NULL;

  /**
   * A list of logged requests.
   */
  public $log = array();

  /**
   * The current request time (UNIX timestamp).
   *
   * @var int
   */
  public $requestTime;

  function __construct() {
    $this->keyPublic = $this->configKeyPublicGet();
    $this->keyPrivate = $this->configKeyPrivateGet();
    $this->requestTime = $this->requestTimeGet();
  }

  abstract public function configKeyPublicGet();

  abstract public function configKeyPrivateGet();

  abstract public function configServerListGet();

  abstract public function configServerListSet(array $servers);

  /**
   * Returns the current request time as UNIX timestamp.
   */
  public function requestTimeGet() {
    return $_SERVER['REQUEST_TIME'];
  }

  /**
   * Writes log messages to a permanent location/storage.
   *
   * Not abstract, since clients are not required to write log messages. There
   * is also no default implementation. However, all clients should permanently
   * store the log messages, as it dramatically improves user support requests.
   * The log may be written and appended to a file (via file_put_contents()),
   * syslog (on *nix-based systems), or a database.
   *
   * @see Mollom::log
   */
  public function writeLog() {
  }

  /**
   * Generate authentication data for XML-RPC communication with Mollom servers.
   *
   * This function generates an array with all information required to
   * authenticate against Mollom.  To prevent forged requests where you are
   * impersonated, each request is signed with a hash based on a private
   * key and a timestamp.
   *
   * Both the client and the server share the secret key used to create
   * the authentication hash.  They both hash a timestamp with the secret
   * key, and if the hashes match, the authenticity of the message is
   * validated.
   *
   * To avoid someone intercepting a (hash, timestamp)-pair and using it
   * to impersonate a client, Mollom reject any request where the timestamp
   * is more than 15 minutes off.
   *
   * Make sure your server's time is synchronized with the world clocks,
   * and that you don't share your private key with anyone else.
   *
   * @param $public_key
   *   (optional) The public key to use for authentication. Only used internally.
   * @param $private_key
   *   (optional) The private key to use for authentication. Only used internally.
   */
  public function getAuthentication() {
    if (empty($this->keyPublic) || empty($this->keyPrivate)) {
      $this->lastResponseCode = MOLLOM_ERROR;
      throw new MollomAuthenticationException('Missing API keys.');
    }

    // Generate a timestamp according to the dateTime format.
    // @see http://www.w3.org/TR/xmlschema-2/#dateTime
    $time = gmdate("Y-m-d\TH:i:s.\\0\\0\\0O", $this->requestTime);

    // Generate a random number.
    $nonce = md5(mt_rand());

    // Calculate a HMAC-SHA1 according to RFC 2104.
    // @see http://www.ietf.org/rfc/rfc2104.txt
    $request_data = $time . ':' . $nonce . ':' . $this->keyPrivate;
    $hash = base64_encode(hash_hmac('sha1', $request_data, $this->keyPrivate, TRUE));

    // Return HMAC authentication parameters as a keyed array.
    $data['publicKey'] = $this->keyPublic;
    $data['time'] = $time;
    $data['hash'] = $hash;
    $data['nonce'] = $nonce;

    return $data;
  }

  /**
   * Fetches Mollom servers from local configuration or retrieves a new list.
   *
   * @todo Consider a magic getter for $this->servers?  I.e., whenever accessed
   *   but not set/undefined, getServers() is automatically invoked.
   */
  public function getServers() {
    // If there is no server list yet, consult the local configuration.
    if (empty($this->servers)) {
      $servers = $this->configServerListGet();
      // Use the local configuration value, if any.
      if (!empty($servers) && is_array($servers)) {
        $this->servers = $servers;
      }
      // Otherwise, retrieve a new server list from Mollom.
      else {
        $this->servers = $this->refreshServers();
        $this->configServerListSet($this->servers);
        if ($this->servers) {
          $this->log[] = array(
            'type' => 'debug',
            'message' => 'Refreshed servers: %servers',
            'arguments' => array(
              '%servers' => implode(', ', $this->servers),
            ),
          );
        }
      }
    }
    return $this->servers;
  }

  public function refreshServers() {
    // refreshServers() cannot use query() as we need to prevent infinite
    // recursion. In addition, we handle returned error codes differently here,
    // since MOLLOM_REDIRECT and MOLLOM_REFRESH, as well as any other
    // communication error requires us to skip to the next server to retrieve a
    // new server list. We only ever abort, if a server returns MOLLOM_ERROR, in
    // which case there must be a configuration error (e.g., invalid API keys).
    $servers = array();
    try {
      $data = $this->getAuthentication();
    }
    catch (MollomAuthenticationException $e) {
      return $servers;
    }

    $path = 'site/' . $this->keyPublic;
    foreach ($this->serversInit as $server) {
      try {
        $result = $this->request('GET', $server, $path, $data);
      }
      catch (MollomAuthenticationException $e) {
        // Bogus configuration. Stop trying, since all servers will fail.
        break;
      }
      catch (MollomException $e) {
        // On any other error, skip to the next server.
        continue;
      }
      // Log any other uncatched error, skip to the next server.
      // @todo Consider to add an $expectedResponseElement parameter directly to
      //   Mollom::query(), passed on to Mollom::request(), in order to be able
      //   to detect an unexpected response where the request is performed?
      if ($this->lastResponseCode !== TRUE || !isset($result['site']['servers'])) {
        $this->log[] = array(
          'type' => 'error',
          'message' => 'Error: @method @uri',
          'arguments' => array(
            '@method' => 'GET',
            '@uri' => $server . '/' . $path,
          ),
          'data' => $data,
          'response' => $result,
        );
        continue;
      }
      // Otherwise, we have a valid result.
      $servers = $result['site']['servers'];
      break;
    }
    return $servers;
  }

  public function query($method, $path, array $data = array()) {
    // Retrieve server list.
    // If we get no list, we will have no servers to iterate over and only the
    // error logic remains.
    // @todo Use a magic getter for $this->servers?
    $this->getServers();

    // Unconditionally add HMAC request authentication parameters.
    try {
      $data += $this->getAuthentication();
    }
    catch (MollomAuthenticationException $e) {
      // Merely catch the exception, move on to error logic below. Relying on
      // getAuthentication() to set a "fake" response code in case no API keys
      // are configured.
    }

    // Initialize refresh variable.
    $refresh = FALSE;
    // Send the request to the first server; if that fails, try the other
    // servers in the list.
    while ($server = current($this->servers)) {
      try {
        $result = $this->request($method, $server, $path, $data);
      }
      catch (MollomRefreshException $e) {
        // Prevent infinite loops.
        if (!$refresh) {
          $refresh = TRUE;

          // Retrieve a new list of valid Mollom servers.
          $this->servers = $this->refreshServers();
          // If API keys are invalid, we won't be able to get a new server list.
          // To reach this, we must have had a server list (and therefore
          // valid keys) before, so we do not immediately return, but trigger
          // the fallback mode instead.
          if (empty($this->servers)) {
            break;
          }
          // Update the server list.
          $this->configServerListSet($this->servers);
          $this->log[] = array(
            'type' => 'debug',
            'message' => '@code: Refreshed servers: %servers',
            'arguments' => array(
              '@code' => $e->getCode(),
              '%servers' => implode(', ', $this->servers),
            ),
          );
        }
      }
      catch (MollomRedirectException $e) {
        // Try the next server in the list.
        $next = next($this->servers);

        $this->log[] = array(
          'type' => 'debug',
          'message' => '@code: Server %server redirected to %next.',
          'arguments' => array(
            '@code' => $e->getCode(),
            '%server' => $server,
            '%next' => $next,
          ),
        );
        continue;
      }
      catch (MollomAuthenticationException $e) {
        $this->log[] = array(
          'type' => 'error',
          'message' => 'Error @code: Invalid authentication or configuration.',
          'arguments' => array(
            '@code' => $e->getCode(),
          ),
        );
        // This is an irrecoverable error, so don't try other servers.
        break;
      }
      catch (MollomException $e) {
        // On any other known error, try the next server.
        next($this->servers);

        $this->log[] = array(
          'type' => 'debug',
          'message' => 'Error @code: @method @uri: %message',
          'arguments' => array(
            '@code' => $e->getCode(),
            '@method' => $method,
            '@uri' => $server . '/' . $path,
            '%message' => $e->getMessage(),
          ),
          'data' => $e->arguments['data'],
          'response' => $e->arguments['response'],
        );
        continue;
      }

      // Unless we have a positive result, continue to next server.
      // @todo Handle uncaught exceptions and errors?
      if ($this->lastResponseCode === TRUE) {
        break;
      }
      else {
        next($this->servers);
      }
    }

    // In case all servers failed, reset the server list to enforce retrieval of
    // a new list the next time.
    if (current($this->servers) === FALSE) {
      $this->servers = array();
      // @todo 
      variable_del('mollom_servers');

      $this->log[] = array(
        'type' => 'error',
        'message' => 'All servers unreachable or returning errors. The server list was emptied.',
      );
    }

    // Write all captured log messages.
    $this->writeLog();

    // If the last request succeeded and the result is an array, return it to
    // the caller.
    // @todo is_array() not yet TRUE for verifyCaptcha yet.
    if ($this->lastResponseCode === TRUE /*&& is_array($result)*/) {
      return $result;
    }
    // Return an authentication error, which may require special client-side
    // processing.
    if ($this->lastResponseCode == MOLLOM_ERROR) {
      return $this->lastResponseCode;
    }

    // In case of any kind of HTTP error (404, 0 [invalid-address],
    // -1002 [bad URI], etc), return a generic NETWORK_ERROR.
    return NETWORK_ERROR;
  }

  /**
   * Performs a HTTP request to a Mollom server.
   *
   * @throws MollomNetworkException
   * @throws MollomAuthenticationException
   * @throws MollomRedirectException
   * @throws MollomRefreshException
   * @throws MollomException
   */
  abstract protected function request($method, $server, $path, array $data);

  /**
   * Builds an RFC-compliant, rawurlencoded query string.
   *
   * PHP did a design decision to only support HTTP query parameters in the form
   * of foo[]=1&foo[]=2, primarily for its built-in and automated conversion to
   * PHP arrays. Other platforms (including the Mollom backend) do not support
   * this syntax and expect multiple parameters to be in the form of
   * foo=1&foo=2.
   *
   * @see http_build_query()
   * @see http://en.wikipedia.org/wiki/Query_string
   * @see http://tools.ietf.org/html/rfc3986#section-3.4
   *
   * @param array $query
   *   The query parameter array to be processed, e.g. $_GET.
   * @param string $parent
   *   Internal use only. Used to build the $query array key for nested items.
   *
   * @return array
   *   A rawurlencoded string which can be used as or appended to the URL query
   *   string.
   *
   * @see Mollom::httpParseQuery()
   */
  public static function httpBuildQuery(array $query, $parent = '') {
    $params = array();

    foreach ($query as $key => $value) {
      // For indexed (unnamed) child array keys, use the same parameter name,
      // leading to param=foo&param=bar instead of param[]=foo&param[]=bar.
      if ($parent && is_int($key)) {
        $key = rawurlencode($parent);
      }
      else {
        $key = ($parent ? $parent . '[' . rawurlencode($key) . ']' : rawurlencode($key));
      }

      // Recurse into children.
      if (is_array($value)) {
        $params[] = self::httpBuildQuery($value, $key);
      }
      // If a query parameter value is NULL, only append its key.
      elseif (!isset($value)) {
        $params[] = $key;
      }
      else {
        // For better readability of paths in query strings, we decode slashes.
        $params[] = $key . '=' . str_replace('%2F', '/', rawurlencode($value));
      }
    }

    return implode('&', $params);
  }

  /**
   * Parses an RFC-compliant, rawurlencoded query string.
   *
   * Mollom clients normally do not need this function, as they do not need to
   * process requests from a server - unless a client attempts to implement
   * client-side unit testing.
   *
   * @param string $query
   *   The query parameter string to process, e.g. $_SERVER['QUERY_STRING']
   *   (GET) or php://input (POST/PUT).
   *
   * @return array
   *   A query parameter array parsed from $query.
   *
   * @see Mollom::httpBuildQuery()
   * @see parse_str()
   */
  public static function httpParseQuery($query) {
    if ($query === '') {
      return array();
    }
    // Explode parameters into arrays to check for duplicate names.
    $params = array();
    $seen = array();
    $duplicate = array();
    foreach (explode('&', $query) as $chunk) {
      $param = explode('=', $chunk, 2);
      if (isset($seen[$param[0]])) {
        $duplicate[$param[0]] = TRUE;
      }
      $seen[$param[0]] = TRUE;
      $params[] = $param;
    }
    // Implode back into a string.
    $query = '';
    foreach ($params as $param) {
      $query .= $param[0];
      if (isset($duplicate[$param[0]])) {
        $query .= '[]';
      }
      if (isset($param[1])) {
        $query .= '=' . $param[1];
      }
      $query .= '&';
    }
    // Parse query string as usual.
    parse_str($query, $result);
    return $result;
  }

  public function getSite($id = NULL) {
    if (!isset($id)) {
      $id = $this->keyPublic;
    }
    $result = $this->query('GET', 'site/' . $id);
    // @todo Error handling?
    // @todo Should we return $result['site'] only? Make ::query() aware of
    //   what key to expect and return only that? Wouldn't work for "list"
    //   responses...
    return isset($result['site']) ? $result['site'] : $result;
  }

  public function verifyKey($data = array()) {
    // @todo Error handling? We expect a certain element in the response.
    $result = $this->query('GET', 'site/' . $this->keyPublic, $data);
    // lastResponseCode will either be TRUE, MOLLOM_ERROR, or NETWORK_ERROR.
    return $this->lastResponseCode === TRUE ? TRUE : $this->lastResponseCode;
  }

  public function checkContent($data = array()) {
    if (!empty($data['contentId'])) {
      $result = $this->query('PUT', 'content/' . $data['contentId'], $data);
    }
    else {
      $result = $this->query('POST', 'content', $data);
    }
    return isset($result['content']) ? $result['content'] : $result;
  }

  public function getCaptcha($data = array()) {
    if (!isset($data['type']) || !in_array($data['type'], array('image', 'audio'))) {
      // @todo Error handling?
      throw new MollomException('Unknown CAPTCHA type.');
    }
    if (!empty($data['captchaId'])) {
      $result = $this->query('PUT', 'captcha/' . $data['captchaId'], $data);
    }
    else {
      $result = $this->query('POST', 'captcha', $data);
    }
    return isset($result['captcha']) ? $result['captcha'] : $result;
  }

  public function checkCaptcha($data = array()) {
    if (empty($data['captchaId'])) {
      // @todo Error handling?
      throw new MollomException('Missing CAPTCHA ID.');
    }
    $result = $this->query('PUT', 'captcha/' . $data['captchaId'], $data);
    return isset($result['captcha']) ? $result['captcha'] : $result;
  }

  /**
   * @todo This part of the REST API needs work. No way to report CAPTCHA sessions.
   */
  public function sendFeedback($data = array()) {
    if (empty($data['contentId'])) {
      // @todo Error handling?
      throw new MollomException('Missing content ID.');
    }
    if (empty($data['moderated'])) {
      // @todo Error handling?
      throw new MollomException('Missing feedback value.');
    }
    $result = $this->query('PUT', 'content/' . $data['contentId'], $data);
    return $this->lastResponseCode === TRUE ? TRUE : FALSE;
  }

  /**
   * @todo List parameters.
   */
  public function getBlacklist($siteId = NULL) {
    if (!isset($siteId)) {
      $siteId = $this->keyPublic;
    }
    $result = $this->query('GET', 'blacklist/' . $siteId);
    // @todo 'list' is a string when empty. Consider to move list response meta
    //   elements into the 'list' element, so it's never empty.
    if (isset($result['list'])) {
      return is_array($result['list']) ? $result['list'] : array();
    }
    return $result;
  }

  public function getBlacklistEntry($entryId, $siteId = NULL) {
    if (!isset($siteId)) {
      $siteId = $this->keyPublic;
    }
    $result = $this->query('GET', 'blacklist/' . $siteId . '/' . $entryId);
    return isset($result['entry']) ? $result['entry'] : $result;
  }

  public function createBlacklistEntry($data = array(), $siteId = NULL) {
    if (!isset($siteId)) {
      $siteId = $this->keyPublic;
    }
    $result = $this->query('POST', 'blacklist/' . $siteId, $data);
    return isset($result['entry']) ? $result['entry'] : $result;
  }

  public function updateBlacklistEntry($data = array(), $siteId = NULL) {
    if (empty($data['id'])) {
      // @todo Error handling?
      throw new MollomException('Missing blacklist entry ID.');
    }
    if (!isset($siteId)) {
      $siteId = $this->keyPublic;
    }
    $result = $this->query('PUT', 'blacklist/' . $siteId . '/' . $data['id'], $data);
    return isset($result['entry']) ? $result['entry'] : $result;
  }

  public function deleteBlacklistEntry($entryId, $siteId = NULL) {
    if (!isset($siteId)) {
      $siteId = $this->keyPublic;
    }
    $result = $this->query('POST', 'blacklist/' . $siteId . '/' . $entryId . '/delete');
    return $this->lastResponseCode === TRUE;
  }
}

/**
 * A potential Mollom client interface.
 *
 * @todo Most clients most probably have a centralized local configuration
 *   storage, so it's likely easier for everyone to handle ->get($what) and
 *   ->set($what) than individual methods.
 */
interface MollomClientInterface {
  /**
   * Returns the current request time as UNIX timestamp.
   */
  public function requestTimeGet();

  /**
   * Returns the public Mollom API key string from local configuration storage.
   */
  public function configKeyPublicGet();

  /**
   * Returns the private Mollom API key string from local configuration storage.
   */
  public function configKeyPrivateGet();

  /**
   * Returns the Mollom server list as an array from local configuration storage.
   */
  public function configServerListGet();

  /**
   * Stores the Mollom server list in local configuration storage.
   */
  public function configServerListSet(array $servers);

}

/**
 * Drupal Mollom client implementation.
 *
 * This class is abstract, because it doesn't implement all final methods (yet).
 */
abstract class MollomDrupal extends Mollom {
  public function requestTimeGet() {
    return REQUEST_TIME;
  }

  public function configKeyPublicGet() {
    return variable_get('mollom_public_key', '');
  }

  public function configKeyPrivateGet() {
    return variable_get('mollom_private_key', '');
  }

  public function configServerListGet() {
    return variable_get('mollom_servers', array());
  }

  public function configServerListSet(array $servers) {
    return variable_set('mollom_servers', $servers);
  }

  public function refreshServers() {
    $servers = parent::refreshServers();

    // Allow other modules to alter the server list. Internal use only.
    drupal_alter('mollom_server_list', $servers);

    return $servers;
  }
  

}

interface MollomProtocolInterface {
}

class MollomDrupalXMLRPC extends MollomDrupal /*implements MollomClientInterface, MollomProtocolInterface*/ {
  /**
   * Mollom API version; used for XML-RPC communication with Mollom servers.
   */
  const API_VERSION = '1.0';

  /**
   * XML-RPC communication failure: No servers could be reached.
   */
  const XMLRPC_ERROR_NETWORK = 900;

  /**
   * XML-RPC communication failure: Error on Mollom server.
   */
  const XMLRPC_ERROR_MOLLOM = 1000;

  /**
   * XML-RPC communication failure: Mollom server requests client to update its server list.
   */
  const XMLRPC_ERROR_REFRESH = 1100;

  /**
   * XML-RPC communication failure: Mollom server defers communication to next server in server list.
   */
  const XMLRPC_ERROR_REDIRECT = 1200;

  function request1($server, $method, array $data) {
    if ($method == 'getCaptcha') {
      $method = ($data['type'] == 'image' ? 'getImageCaptcha' : 'getAudioCaptcha');
      unset($data['type']);
    }

    $server .= '/' . self::API_VERSION;
    $method = 'mollom.' . $method;
    $result = xmlrpc($server, array(
      $method => array($data),
    ));
    if ($error = xmlrpc_error()) {
      $this->lastResponseCode = $error->code;
      $this->log[] = array(
        'type' => 'error',
        'message' => 'Error @code: @method: %message',
        'arguments' => array(
          '@code' => $error->code,
          '@method' => $server . '/' . $method,
          '%message' => $error->message,
        ),
        'data' => $data,
        'response' => $result,
      );
    }
    else {
      $this->lastResponseCode = TRUE;
      $this->log[] = array(
        'type' => 'debug',
        'message' => '@method',
        'arguments' => array(
          '@method' => $server . '/' . $method,
        ),
        'data' => $data,
        'response' => $result,
      );
      return $result;
    }
  }

  protected function request($method, $server, $path, array $data) {
  }

}

class MollomDrupalREST extends MollomDrupal implements MollomProtocolInterface {
  /**
   * Mollom API version.
   */
  const API_VERSION = 'v1';

  public $serversInit = array('http://megatron.mollom.com');

  /**
   * Overrides Mollom::writeLog().
   */
  function writeLog() {
    $messages = array();
    foreach ($this->log as $entry) {
      $entry += array('arguments' => array());
      $message = array(
        $entry['message'] => $entry['arguments'],
      );
      if (isset($entry['data'])) {
        $message['Request:<pre>@request</pre>'] = array('@request' => $entry['data']);
      }
      if (isset($entry['response'])) {
        $message['Response:<pre>@response</pre>'] = array('@response' => $entry['response']);
      }
      $messages[] = $message;
    }
    _mollom_watchdog_multiple($messages, $this->lastResponseCode === TRUE ? WATCHDOG_DEBUG : WATCHDOG_ERROR);
  }

  /**
   * Implements Mollom::request().
   */
  protected function request($method, $server, $path, array $data) {
    // Build the REST request path.
    $path = self::API_VERSION . '/' . $path;
    $rest_path = $path;
    $query = NULL;
    if ($data) {
      $query = $this->httpBuildQuery($data);
      if (in_array($method, array('GET', 'HEAD'))) {
        $rest_path .= '?' . $query;
      }
    }

    $request_data = array(
      'method' => $method,
      'headers' => array(
        'Accept' => 'application/xml, application/json;q=0.8, */*;q=0.5',
      ),
    );
    if (in_array($method, array('POST', 'PUT'))) {
      $request_data['data'] = $query;
      $request_data['headers']['Content-Type'] = 'application/x-www-form-urlencoded';
    }

    $dhr = drupal_http_request($server . '/' . $rest_path, $request_data);
    // @todo Core: Any other code than 200 as interpreted as error.
    $error = (isset($dhr->error) && $dhr->code[0] != 2);
    // @todo Core: data property is not assigned if there is no response body.
    if (!isset($dhr->data)) {
      $dhr->data = NULL;
    }

    // Parse the response data.
    if (isset($dhr->headers['content-type'])) {
      if (strstr($dhr->headers['content-type'], 'application/json')) {
        $dhr->data = drupal_json_decode($dhr->data);
      }
      // Before attempting to parse XML, ensure the response actually is XML.
      //  && is_string($dhr->data) && isset($dhr->data[1]) && $dhr->data[1] === '?'
      elseif (strstr($dhr->headers['content-type'], 'application/xml')) {
        $dhr->elements = new SimpleXmlIterator($dhr->data);
        $dhr->data = $this->parseXML($dhr->elements);
      }
    }

    // If a 'code' exists in the response, it has precedence, regardless of a
    // possible HTTP error code.
    if (isset($dhr->data) && is_array($dhr->data) && isset($dhr->data['code']) && $dhr->data['code'] != 0) {
      $error = TRUE;
      // Replace HTTP status code with 'code' from response.
      $dhr->code = $dhr->data['code'];
      // If there is no HTTP status message, take over 'message' from response.
      // @todo Remove this condition when Mollom sends proper error messages.
      if (!isset($dhr->error)) {
        $dhr->error = $dhr->data['message'];
      }
    }

    // Ensure that $dhr->code is an integer.
    $dhr->code = (int) $dhr->code;

    if ($error) {
      $this->log[] = array(
        'type' => 'error',
        'message' => 'Error @code: @method @uri: %message',
        'arguments' => array(
          '@code' => $dhr->code,
          '@method' => $method,
          '@uri' => $server . '/' . $path,
          '%message' => $dhr->error,
        ),
        'data' => $data,
        'response' => $dhr->data,
      );
      $arguments = array(
        'data' => $data,
        'response' => $dhr->data,
      );
      if ($dhr->code <= 0) {
        $this->lastResponseCode = NETWORK_ERROR;
        throw new MollomNetworkException(t('Network error.'), NETWORK_ERROR, NULL, $arguments);
      }
      if ($dhr->code == 1 || $dhr->code == MOLLOM_ERROR || $dhr->code == 401) {
        $this->lastResponseCode = MOLLOM_ERROR;
        throw new MollomAuthenticationException(t('Invalid authentication.'), MOLLOM_ERROR, NULL, $arguments);
      }
      if ($dhr->code == 2 || $dhr->code == MOLLOM_REDIRECT || ($dhr->code >= 300 && $dhr->code < 400)) {
        $this->lastResponseCode = MOLLOM_REDIRECT;
        throw new MollomRedirectException(t('Server redirected to next.'), MOLLOM_REDIRECT, NULL, $arguments);
      }
      if ($dhr->code == 3 || $dhr->code == MOLLOM_REFRESH || $dhr->code == 503) {
        $this->lastResponseCode = MOLLOM_REFRESH;
        throw new MollomRefreshException(t('Refresh of server list required.'), MOLLOM_REFRESH, NULL, $arguments);
      }
      $this->lastResponseCode = $dhr->code;
      throw new MollomException($dhr->error, $dhr->code, NULL, $arguments);
    }
    else {
      $this->lastResponseCode = TRUE;
      $this->log[] = array(
        'type' => 'debug',
        'message' => '@code @method @uri',
        'arguments' => array(
          '@code' => $dhr->code,
          '@method' => $method,
          '@uri' => $server . '/' . $path,
        ),
        'data' => $data,
        'response' => $dhr->data,
      );

      return $dhr->data;
    }
  }

  /**
   * Converts a SimpleXmlIterator structure into an associative array.
   */
  protected function parseXML(SimpleXMLIterator $sxi) {
    $a = array();
    $remove = array();
    for ($sxi->rewind(); $sxi->valid(); $sxi->next()) {
      $key = $sxi->key();
      // Convert CamelCase to lowercase with underscores.
      #$key = strtolower(preg_replace('@(?<=[a-z])([A-Z])@', '_$1', $key));

      // Recurse into non-scalar values.
      if ($sxi->hasChildren()) {
        $a[$key] = $this->parseXML($sxi->current());
      }
      // Use a simple key/value pair for scalar values.
      else {
        if (!isset($a[$key])) {
          $a[$key] = strval($sxi->current());
        }
        // Convert already existing keys into indexed keys, retaining other
        // existing keys in the array; i.e., two or more XML elements of the
        // same name and on the same level.
        // Note that this XML to PHP array conversion does not support different
        // elements that each appear multiple times.
        else {
          // First time we reach here, convert the existing keyed item. Do not
          // remove $key, so we enter this path again.
          if (!isset($remove[$key])) {
            $a[] = $a[$key];
            // Mark $key for removal.
            $remove[$key] = $key;
          }
          // Add the new item.
          $a[] = strval($sxi->current());
        }
      }
    }
    foreach ($remove as $key) {
      unset($a[$key]);
    }
    return $a;
  }

}

class MollomDrupalRESTTest extends MollomDrupalREST {
  /**
   * Overrides Mollom::__construct().
   */
  function __construct() {
    // Replace initial server list with local fake server.
    $this->serversInit = array($GLOBALS['base_url'] . '/mollom-test/rest');
    parent::__construct();
  }
}

