<?php

/**
 * @file
 * Mollom client class for Drupal.
 */

/**
 * Drupal Mollom client implementation.
 */
class MollomDrupal extends Mollom {
  /**
   * Mapping of configuration names to Drupal variables.
   *
   * @see Mollom::loadConfiguration()
   */
  private $configuration_map = array(
    'publicKey' => 'mollom_public_key',
    'privateKey' => 'mollom_private_key',
    'servers' => 'mollom_servers',
  );

  /**
   * Implements Mollom::loadConfiguration().
   */
  public function loadConfiguration($name) {
    $name = $this->configuration_map[$name];
    return variable_get($name);
  }

  /**
   * Implements Mollom::saveConfiguration().
   */
  public function saveConfiguration($name, $value) {
    $name = $this->configuration_map[$name];
    return variable_set($name, $value);
  }

  /**
   * Implements Mollom::deleteConfiguration().
   */
  public function deleteConfiguration($name) {
    $name = $this->configuration_map[$name];
    return variable_del($name);
  }

  /**
   * Implements Mollom::getClientInformation().
   */
  public function getClientInformation() {
    if ($cache = cache_get('mollom_version')) {
      return $cache->data;
    }

    // Retrieve Drupal distribution and installation profile information.
    $profile = drupal_get_profile();
    $profile_info = system_get_info('module', $profile) + array(
      'distribution_name' => 'Drupal',
      'version' => VERSION,
    );

    // Retrieve Mollom module information.
    $mollom_info = system_get_info('module', 'mollom');
    if (empty($mollom_info['version'])) {
      // Manually build a module version string for repository checkouts.
      $mollom_info['version'] = DRUPAL_CORE_COMPATIBILITY . '-1.x-dev';
    }

    $data = array(
      'platformName' => $profile_info['distribution_name'],
      'platformVersion' => $profile_info['version'],
      'clientName' => $mollom_info['name'],
      'clientVersion' => $mollom_info['version'],
    );
    cache_set('mollom_version', $data);

    return $data;
  }

  /**
   * Overrides Mollom::writeLog().
   */
  function writeLog() {
    $messages = array();
    foreach ($this->log as $i => $entry) {
      $entry += array('arguments' => array());
      $message = array(
        $entry['message'] => $entry['arguments'],
      );
      if (isset($entry['data'])) {
        $message['Request: @request<pre>@parameters</pre>'] = array(
          '@request' => $entry['request'],
          '@parameters' => $entry['data'],
        );
        unset($entry['request'], $entry['data']);
      }
      if (isset($entry['response'])) {
        $message['Response:<pre>@response</pre>'] = array('@response' => $entry['response']);
      }
      $messages[] = $message;

      // Translate log messages for debugging without watchdog.
      // @todo Temporary?
      $output = array();
      foreach ($message as $text => $args) {
        foreach ($args as &$arg) {
          if (is_array($arg)) {
            $arg = var_export($arg, TRUE);
          }
        }
        $output[] = strtr($text, $args);
      }
      $this->log[$i]['message'] = implode("\n", $output);
      unset($this->log[$i]['arguments']);
//      drupal_set_message(implode('<br />', $output), $this->log[$i]['type']);
    }
    _mollom_watchdog_multiple($messages, $this->lastResponseCode === TRUE ? WATCHDOG_DEBUG : WATCHDOG_ERROR);

    // After writing log messages, empty the log.
    $this->purgeLog();
  }

  /**
   * Overrides Mollom::refreshServers().
   */
  protected function refreshServers() {
    $servers = parent::refreshServers();

    // Allow other modules to alter the server list. Internal use only.
    drupal_alter('mollom_server_list', $servers);

    return $servers;
  }

  /**
   * Implements Mollom::request().
   */
  protected function request($method, $server, $path, array $data, array $expected = array()) {
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

    // Parse the response body into a PHP array.
    if (isset($dhr->headers['content-type'])) {
      if (strstr($dhr->headers['content-type'], 'application/json')) {
        $dhr->data = drupal_json_decode($dhr->data);
      }
      elseif (strstr($dhr->headers['content-type'], 'application/xml')) {
        $dhr->elements = new SimpleXmlIterator($dhr->data);
        $dhr->data = $this->parseXML($dhr->elements);
      }
    }

    // @todo The above is all that may vary between client implementations. All
    //   of the below logic should live in a separate method that clients don't
    //   have to re-implement:
    //     handleResponse($http_code, $http_message, $response)
    //   Or alternatively, since handleResponse() is required for every request,
    //   move the above into a new sub-method invoked from request():
    //     executeRequest($method, $server, $path, array $data)
    //   and make it return $http_code, $http_message, array $response.

    // If a 'code' exists in the response, it has precedence, regardless of a
    // possible HTTP error code.
    if (isset($dhr->data) && is_array($dhr->data) && isset($dhr->data['code']) && $dhr->data['code'] != 0) {
      $error = TRUE;
      // Replace HTTP status code with 'code' from response.
      $dhr->code = $dhr->data['code'];
      // If there is no HTTP status message, take over 'message' from response.
      // @todo Remove this condition when Mollom sends proper error messages.
      if (!isset($dhr->error) && isset($dhr->data['message'])) {
        $dhr->error = $dhr->data['message'];
      }
    }

    // Verify that an expected element exists in the response.
    // Normally, a response validation like this would likely be done using XML
    // schemas, so the XML parser would already (in)validate the response.
    // However, when using the JSON protocol, there is no comparable notion of a
    // schema. Instead of checking for only one particular element in the
    // response, the client could also introduce dedicated classes for each
    // resource, but that feels like over-engineering for the average use-case.
    $key_exists = TRUE;
    if (!$error && !empty($expected)) {
      $key_exists = FALSE;
      if (is_array($dhr->data)) {
        $key_exists = $this->arrayKeyExists($dhr->data, $expected);
      }
      if (!$key_exists) {
        $error = TRUE;
        $dhr->code = self::RESPONSE_ERROR;
        $dhr->error = 'Unexpected server response.';
      }
    }

    // Ensure that $dhr->code is an integer.
    $dhr->code = (int) $dhr->code;

    if ($error) {
      $arguments = array(
        'request' => $method . ' ' . $server . '/' . $path,
        'data' => $data,
        'response' => $dhr->data,
      );
      if ($dhr->code <= 0) {
        throw new MollomNetworkException(t('Network error.'), self::NETWORK_ERROR, NULL, $this, $arguments);
      }
      if ($dhr->code == 1 || $dhr->code == self::AUTH_ERROR || $dhr->code == 401) {
        throw new MollomAuthenticationException(t('Invalid authentication.'), self::AUTH_ERROR, NULL, $this, $arguments);
      }
      if ($dhr->code == 2 || $dhr->code == self::REDIRECT_ERROR || ($dhr->code >= 300 && $dhr->code < 400)) {
        throw new MollomRedirectException(t('Server redirected to next.'), self::REDIRECT_ERROR, NULL, $this, $arguments);
      }
      if ($dhr->code == 3 || $dhr->code == self::REFRESH_ERROR) {
        throw new MollomRefreshException(t('Refresh of server list required.'), self::REFRESH_ERROR, NULL, $this, $arguments);
      }
      if (!$key_exists || $dhr->code >= 500) {
        throw new MollomResponseException(t('Unexpected server response.'), self::RESPONSE_ERROR, NULL, $this, $arguments);
      }
      throw new MollomException($dhr->error, $dhr->code, NULL, $this, $arguments);
    }
    else {
      $this->lastResponseCode = TRUE;
      $this->log[] = array(
        'type' => 'debug',
        'message' => '@code @method @uri',
        'arguments' => array(
          '@code' => $dhr->code,
          '@method' => $method,
          '@uri' => $path,
        ),
        'request' => $method . ' ' . $server . '/' . $path,
        'data' => $data,
        'response' => $dhr->data,
      );

      return $dhr->data;
    }
  }
}

/**
 * Drupal Mollom client implementation using production testing servers.
 */
class MollomDrupalTest extends MollomDrupal {
  /**
   * Overrides Mollom::$serversInit.
   */
  public $serversInit = array('http://megatron.mollom.com');

  /**
   * @todo Backend: Returned server list contains API version.
   */
  protected function refreshServers() {
    $servers = parent::refreshServers();
    foreach ($servers as &$server) {
      if (substr($server, -3) == '/v1') {
        $server = substr($server, 0, -3);
      }
    }
    return $servers;
  }
}

/**
 * Drupal Mollom client implementation using local dummy/fake REST server.
 */
class MollomDrupalTestLocal extends MollomDrupal {
  /**
   * Overrides Mollom::__construct().
   */
  function __construct() {
    parent::__construct();
    // Replace initial server list with local fake server.
    $this->serversInit = array($GLOBALS['base_url'] . '/mollom-test/rest');
  }
}

/**
 * Drupal Mollom client implementation talking XML-RPC.
 *
 * @todo Incomplete.
 */
class MollomDrupalXMLRPC extends MollomDrupal {
  /**
   * Overrides Mollom::API_VERSION for XML-RPC requests.
   */
  const API_VERSION = '1.0';

  /**
   * Overrides Mollom::serversInit for XML-RPC requests.
   */
  public $serversInit = array('http://xmlrpc1.mollom.com', 'http://xmlrpc2.mollom.com', 'http://xmlrpc3.mollom.com');

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

  protected function request($method, $server, $path, array $data, array $expected = array()) {
  }
}

