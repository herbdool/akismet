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
 *   - No clue how MOLLOM_REFRESH and MOLLOM_REDIRECT will look in REST yet.
 * - More specific MollomException sub-classes? Instead of setting a
 *   lastResponseCode, there could be MollomRedirectException aso.; however,
 *   exceptions require try/catch blocks in _all_ calling code.
 * - Doing a "verifyKey" with an empty server list leads to two/duplicate and
 *   unnecessary GET site/$id requests, one for the server list, subsequent ones
 *   for whatever/verifyKey. Some resources can be cached.
 */

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
      throw new MollomException('Missing API keys.');
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
    foreach ($this->serversInit as $server) {
      $result = $this->request('GET', $server, 'site/' . $this->keyPublic, $this->getAuthentication());
      if ($this->lastResponseCode !== TRUE || !isset($result['site']['servers'])) {
        // A MOLLOM_ERROR indicates a bogus configuration. In this case, stop
        // trying, since all servers will fail.
        if ($this->lastResponseCode == MOLLOM_ERROR) {
          break;
        }
        // Otherwise, skip to the next server.
        else {
          continue;
        }
      }
      // Otherwise, we have a valid result.
      else {
        $servers = $result['site']['servers'];
        break;
      }
    }
    return $servers;
  }

  public function query1($method, array $data = array()) {
    // Enable testing mode.
    if (variable_get('mollom_testing_mode', 0)) {
      $data['testing'] = TRUE;
    }

    // Retrieve server list.
    // @todo Use a magic getter for $this->server?
    $this->getServers();
    // If we get no list, return the error code.
    // @todo Better error handling.
    if (empty($this->servers)) {
      return $this->lastResponseCode;
    }

    // Unconditionally add HMAC request authentication parameters.
    $data += $this->getAuthentication();

    // Initialize refresh variable.
    $refresh = FALSE;
    // Send the request to the first server; if that fails, try the other
    // servers in the list.
    reset($this->servers);
    while ($server = current($this->servers)) {
      $result = $this->request1($server, $method, $data);

      // Handle non-positive request result.
      // @todo Make request() throw exceptions instead?
      if ($this->lastResponseCode !== TRUE) {
        if ($this->lastResponseCode === MOLLOM_REFRESH) {
          // Avoid endless loops.
          if (!$refresh) {
            $refresh = TRUE;

            // Retrieve a new list of valid Mollom servers.
            $this->servers = $this->refreshServers();
            // Update the server list.
            $this->configServerListSet($this->servers);
            // If API keys are invalid, the XML-RPC error code is returned.
            // To reach this, we must have had a server list (and therefore
            // valid keys) before, so we do not immediately return (like above),
            // but instead trigger the fallback mode.
            if (empty($this->servers)) {
              break;
            }
            $this->log[] = array(
              'type' => 'debug',
              'message' => 'Refreshed servers: %servers',
              'arguments' => array(
                '%servers' => implode(', ', $this->servers),
              ),
            );
            // No reset() required, because:
            // php -r "$a = array(1, 2, 3); $i = 0; while ($b = current($a)) { var_dump($b); if ($b == 2 && !$i) {$i = 1; $a = array(1, 2, 3); continue; } next($a); }"
            // => int(1), int(2), int(1), int(2), int(3)
          }
        }
        elseif ($this->lastResponseCode === MOLLOM_REDIRECT) {
          // Try the next server in the list.
          $next = next($this->servers);

          $this->log[] = array(
            'type' => 'debug',
            'message' => 'Server %server redirected to: %next.',
            'arguments' => array(
              '%server' => $server,
              '%next' => $next,
            ),
          );
        }
        // Instantly return upon a 'real' error.
        elseif ($this->lastResponseCode === MOLLOM_ERROR) {
          return MOLLOM_ERROR;
        }
        // On any other error, try the next server.
        else {
          next($this->servers);
        }
      }
      // Positive result.
      else {
        # _mollom_watchdog_multiple($messages, WATCHDOG_DEBUG);
        return $result;
      }
    }

    // If everything failed, we reset the server list to force Mollom to request
    // a new list.
    variable_del('mollom_servers');

    // Report this error.
    $this->log[] = array(
      'type' => 'debug',
      'message' => 'All servers unreachable or returning errors. The server list was emptied.',
    );
    # _mollom_watchdog_multiple($messages, WATCHDOG_ERROR);

    return NETWORK_ERROR;
  }

  public function query($method, $path, array $data = array()) {
    // Enable testing mode.
    if (variable_get('mollom_testing_mode', 0)) {
      $data['testing'] = TRUE;
    }

    // Retrieve server list.
    // @todo Use a magic getter for $this->server?
    $this->getServers();
    // If we get no list, return the error code.
    // @todo Better error handling.
    if (empty($this->servers)) {
      return $this->lastResponseCode;
    }

    // Unconditionally add HMAC request authentication parameters.
    $data += $this->getAuthentication();

    // Initialize refresh variable.
    $refresh = FALSE;
    // Send the request to the first server; if that fails, try the other
    // servers in the list.
    reset($this->servers);
    while ($server = current($this->servers)) {
      $result = $this->request($method, $server, $path, $data);

      // Handle non-positive request result.
      // @todo Make request() throw exceptions instead?
      if ($this->lastResponseCode !== TRUE) {
        if ($this->lastResponseCode === MOLLOM_REFRESH) {
          // Avoid endless loops.
          if (!$refresh) {
            $refresh = TRUE;

            // Retrieve a new list of valid Mollom servers.
            $this->servers = $this->refreshServers();
            // Update the server list.
            $this->configServerListSet($this->servers);
            // If API keys are invalid, the XML-RPC error code is returned.
            // To reach this, we must have had a server list (and therefore
            // valid keys) before, so we do not immediately return (like above),
            // but instead trigger the fallback mode.
            if (empty($this->servers)) {
              break;
            }
            $this->log[] = array(
              'type' => 'debug',
              'message' => 'Refreshed servers: %servers',
              'arguments' => array(
                '%servers' => implode(', ', $this->servers),
              ),
            );
            // No reset() required, because:
            // php -r "$a = array(1, 2, 3); $i = 0; while ($b = current($a)) { var_dump($b); if ($b == 2 && !$i) {$i = 1; $a = array(1, 2, 3); continue; } next($a); }"
            // => int(1), int(2), int(1), int(2), int(3)
          }
        }
        elseif ($this->lastResponseCode === MOLLOM_REDIRECT) {
          // Try the next server in the list.
          $next = next($this->servers);

          $this->log[] = array(
            'type' => 'debug',
            'message' => 'Server %server redirected to: %next.',
            'arguments' => array(
              '%server' => $server,
              '%next' => $next,
            ),
          );
        }
        // Instantly return upon a 'real' error.
        elseif ($this->lastResponseCode === MOLLOM_ERROR) {
          return MOLLOM_ERROR;
        }
        // On any other error, try the next server.
        else {
          next($this->servers);
        }
      }
      // Positive result.
      else {
        # _mollom_watchdog_multiple($messages, WATCHDOG_DEBUG);
        return $result;
      }
    }

    // If everything failed, we reset the server list to force Mollom to request
    // a new list.
    variable_del('mollom_servers');

    // Report this error.
    $this->log[] = array(
      'type' => 'debug',
      'message' => 'All servers unreachable or returning errors. The server list was emptied.',
    );
    # _mollom_watchdog_multiple($messages, WATCHDOG_ERROR);

    return NETWORK_ERROR;
  }

  abstract function request1($server, $method, array $data);

  public function getSite($id = NULL) {
    if (!isset($id)) {
      $id = $this->keyPublic;
    }
    $result = $this->query('GET', 'site/' . $id);
    // @todo Error handling?
    return isset($result['site']) ? $result['site'] : $result;
  }
}

/**
 * MollomException class.
 */
class MollomException extends Exception {
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

}

class MollomDrupalREST extends MollomDrupal implements MollomProtocolInterface {
  /**
   * Mollom API version.
   */
  const API_VERSION = 'v1';

  public $serversInit = array('http://megatron.mollom.com/mollomresttest/myrest');

  function request1($server, $method, array $data) {
    // Map methods to REST paths.
    $original_method = $method;
    switch ($method) {
      case 'getServerList':
        $method = 'GET';
        $path = 'site/' . $this->keyPublic  . '/serverlist';
        break;

      case 'verifyKey':
        $method = 'GET';
        $path = 'site/' . $this->keyPublic  . '/verify';
        break;

      case 'checkContent':
        $method = 'POST';
        $path = 'content';
        break;

      case 'getCaptcha':
      case 'getImageCaptcha':
      case 'getAudioCaptcha':
        $method = 'POST';
        $path = 'captcha/' . $data['type'];
        break;

      case 'checkCaptcha':
        $method = 'POST';
        $path = 'captcha/' . $data['session_id'] . '/verify';
        break;

      case 'sendFeedback':
        $method = 'POST';
        $path = 'content/' . $data['session_id'] . '/feedback';
        break;

      case 'listBlacklistText':
      case 'listBlacklistURL':
        $method = 'GET';
        $path = 'blacklist/' . $this->keyPublic;
        break;

      case 'addBlacklistText':
      case 'addBlacklistURL':
        $method = 'POST';
        $path = 'blacklist/' . $this->keyPublic;
        break;

      case 'removeBlacklistText':
      case 'removeBlacklistURL':
        $method = 'POST';
        $path = 'blacklist/' . $this->keyPublic . '/delete';
        break;

      default:
        $path = $method;
        $method = 'GET';
        #throw new MollomException("Unknown method $method.");
        break;
    }

    $result = $this->request($method, $server, $path, $data);

    if (isset($result)) {
      // Clean-up.
      if ($original_method == 'verifyKey') {
        $result = (isset($result['code']) && $result['code'] == 0);
      }
      if ($original_method == 'getServerList') {
        $result = $result['serverlist'];

        // @todo Permanent static replacement with testing server.
        $result = array($server);
      }
      // No XML value data types; /rest/captcha/verify returns a string
      // instead of a Boolean.
      if ($original_method == 'checkCaptcha') {
        $result = ($result['result'] == 'true');
      }
      // New 'code' response key moves certain response values into sub-keys.
      if ($original_method == 'listBlacklistText' || $original_method == 'listBlacklistURL') {
        // Response contains empty string if blacklist is empty.
        $result = (isset($result['entries']) && is_array($result['entries']) ? $result['entries'] : array());
      }
      // New 'code' response key...
      if (in_array($original_method, array('addBlacklistText', 'addBlacklistURL', 'removeBlacklistText', 'removeBlacklistURL'))) {
        $result = !(bool) $result['code'];
      }

      return $result;
    }
  }

  function request($method, $server, $path, array $data) {
    // Build the REST request path.
    $path = self::API_VERSION . '/' . $path;
    $rest_path = $path;
    $query = NULL;
    if ($data) {
      $query = drupal_http_build_query($data);
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
    // drupal_http_request() interprets any other code than 200 as error.
    $error = (isset($dhr->error) && $dhr->code[0] != 2);

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

    // 'code' != 0 in the response denotes a MOLLOM_ERROR.
    if (!$error && isset($dhr->data)) {
      if (is_array($dhr->data) && isset($dhr->data['code']) && $dhr->data['code'] != 0) {
        $error = TRUE;
        // If there is no HTTP error code, take over 'code' from response.
        if (!isset($dhr->code)) {
          $dhr->code = (int) $dhr->data['code'];
        }
        // If there is no HTTP status message, take over 'message' from response.
        if (!isset($dhr->error)) {
          $dhr->error = $dhr->data['message'];
        }
      }
    }
    if ($error) {
      $this->lastResponseCode = $dhr->code;
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

  function verifyKey($data) {
    $result = $this->query('GET', 'site/' . $this->keyPublic);
    // @todo Error handling?
    return is_array($result) && isset($result['site']) ? TRUE : FALSE;
  }

}

class MollomDrupalRESTTest extends MollomDrupalREST {
  /**
   * Overrides Mollom::__construct().
   */
  function __construct() {
    // Replace initial server list with local fake server.
    $this->serversInit = array($GLOBALS['base_url'] . '/mollom-test/rest');
    // Replace keys with testing keys.
    module_load_include('test', 'mollom', 'tests/mollom');
    $this->keyPublic = MOLLOM_TEST_PUBLIC_KEY;
    $this->keyPrivate = MOLLOM_TEST_PRIVATE_KEY;

    $this->requestTime = $this->requestTimeGet();
  }
}

