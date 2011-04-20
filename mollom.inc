<?php

/**
 * @file
 * Mollom client/server interaction functions.
 *
 * @todo
 * - Abstract MOLLOM_ERROR request response handling.
 * - MollomException class[es].
 * - $serversInit override.
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
  protected $serversInit = array('http://xmlrpc1.mollom.com', 'http://xmlrpc2.mollom.com', 'http://xmlrpc3.mollom.com');

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
    $data['public_key'] = $this->keyPublic;
    $data['time'] = $time;
    $data['hash'] = $hash;
    $data['nonce'] = $nonce;

    return $data;
  }

  public function getServerList() {
    // If there is no server list yet, consult the local configuration.
    if (empty($this->servers)) {
      $servers = $this->configServerListGet();
      // Use the local configuration value, if any.
      if (0 && !empty($servers)) {
        $this->servers = $servers;
      }
      // Otherwise, retrieve a new server list from Mollom.
      else {
        $this->servers = $this->refreshServers();
      }
    }
    return $this->servers;
  }

  public function refreshServers() {
    // mollom.getServerList cannot use mollom() as we need to prevent infinite
    // recursion. In addition, we handle returned error codes differently here,
    // since MOLLOM_REDIRECT and MOLLOM_REFRESH, as well as any other
    // communication error requires us to skip to the next server to retrieve a
    // new server list. We only ever abort, if a server returns MOLLOM_ERROR, in
    // which case there must be a configuration error (e.g., invalid API keys).
    $servers = array();
    foreach ($this->serversInit as $server) {
      $result = $this->request($server, 'getServerList', $this->getAuthentication());
      if ($this->lastResponseCode !== TRUE) {
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
        $servers = $result;
        break;
      }
    }
    return $servers;
  }

  abstract function request($server, $method, $data);

  function checkContent() {
  }
  
  
}

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

}

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

  function request($server, $method, $data) {
    $server .= '/' . self::API_VERSION;
    $method = 'mollom.' . $method;
    $result = xmlrpc($server, array($method => array($data)));
    if ($error = xmlrpc_error()) {
      $this->lastResponseCode = $error->code;
      $this->log[] = array(
        'type' => 'error',
        'message' => 'Error @code on %method: %message',
        'arguments' => array(
          '@code' => $error->code,
          '%method' => $server . '/' . $method,
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
        'message' => '%method',
        'arguments' => array(
          '%method' => $server . '/' . $method,
        ),
        'data' => $data,
        'response' => $result,
      );
      return $result;
    }
  }

}

class MollomDrupalREST extends MollomDrupal implements MollomProtocolInterface {
  function configServerListGet() {
    return variable_get('mollom_servers_rest', array());
  }

  function request($server, $method, $data) {
    // Map methods to REST paths.
    $original_method = $method;
    switch ($method) {
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
        throw new MollomException('Unknown method.');
        break;
    }

    // Build the REST request path.
    $rest_path = $path;
    $query = NULL;
    if ($data) {
      $query = drupal_http_build_query($data);
      if ($method != 'POST') {
        $rest_path .= '?' . $query;
      }
    }

    $request_data = array(
      'method' => $method,
      'headers' => array(
        'Accept' => 'application/xml, application/json;q=0.8, */*;q=0.5',
      ),
      // Required for POST requests.
      'data' => $query,
    );
    if ($method == 'POST') {
      $request_data['headers']['Content-Type'] = 'application/x-www-form-urlencoded';
    }

    $dhr = drupal_http_request($server . '/' . $rest_path, $request_data);
    // drupal_http_request() interprets any other code than 200 as error.
    $error = (isset($dhr->error) && $dhr->code[0] != 2);
    // 'code' != 0 in the response denotes a MOLLOM_ERROR.
    if (!$error && isset($dhr->data)) {
      // ... parse response...
      // $result = $this->parseXML($dhr->data);
      if (isset($result) && is_array($result) && isset($result['code']) && $result['code'] != 0) {
        $error = TRUE;
        $dhr->code = (int) $result['code'];
      }
    }
    if ($error) {
      $this->lastResponseCode = $dhr->code;
      $this->log[] = array(
        'type' => 'error',
        'message' => 'Error @code on %method: %message',
        'arguments' => array(
          '@code' => $dhr->code,
          '%method' => $method . ' ' . $server . '/' . $path,
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
        'message' => '%method',
        'arguments' => array(
          '%method' => $method . ' ' . $server . '/' . $path,
        ),
        'data' => $data,
        'response' => $dhr->data,
      );
      return $dhr->data;
    }
  }

}


function _mollom_authentication($public_key = NULL, $private_key = NULL) {
  $mollom = new MollomDrupal();
  if (isset($public_key)) {
    $mollom->keyPublic = $public_key;
  }
  if (isset($private_key)) {
    $mollom->keyPrivate = $private_key;
  }
  return $mollom->getAuthentication();
}

/**
 * Refreshes the list of Mollom's XML-RPC servers.
 */
function _mollom_retrieve_server_list() {
  // Start from a hard-coded list of servers.
  $servers = array('http://xmlrpc1.mollom.com', 'http://xmlrpc2.mollom.com', 'http://xmlrpc3.mollom.com');
  $messages = array();

  // mollom.getServerList cannot use mollom() as we need to prevent infinite
  // recursion. In addition, we handle returned error codes differently here,
  // since MOLLOM_REDIRECT and MOLLOM_REFRESH, as well as any other
  // communication error requires us to skip to the next server to retrieve a
  // new server list. We only ever abort, if a server returns MOLLOM_ERROR, in
  // which case there must be a configuration error (e.g., invalid API keys).
  $method = 'mollom.getServerList';
  foreach ($servers as $server) {
    $result = xmlrpc($server . '/' . MOLLOM_API_VERSION, array($method => array(_mollom_authentication())));
    if ($result === FALSE && ($error = xmlrpc_error())) {
      // In any case, log the error.
      $messages[] = array(
        'Error @errno from %server for %method: %message' => array(
          '@errno' => $error->code,
          '%server' => $server,
          '%method' => $method,
          '%message' => $error->message,
        ),
      );
      // Skip to the next server in case of any error, except if we have a
      // MOLLOM_ERROR, which indicates a bogus configuration. In this case, stop
      // trying, since all servers will fail.
      if ($error->code === MOLLOM_ERROR) {
        break;
      }
    }
    // Otherwise, we have a valid result.
    else {
      break;
    }
  }

  // Allow other modules to alter the server list. Internal use only.
  drupal_alter('mollom_server_list', $result);

  if (is_array($result)) {
    _mollom_watchdog_multiple($messages, WATCHDOG_DEBUG);
    return $result;
  }
  else {
    _mollom_watchdog_multiple($messages, WATCHDOG_ERROR);
    return xmlrpc_errno();
  }
}

