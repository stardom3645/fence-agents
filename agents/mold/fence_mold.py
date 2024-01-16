#!/usr/bin/python3 -tt
import base64
import hashlib
import hmac
import json
import ssl
import sys
import logging
import atexit
import syslog
import urllib

sys.path.append("/usr/share/fence")
from fencing import *
from fencing import fail, fail_usage, run_delay, EC_STATUS, run_command, SyslogLibHandler

import requests
from requests import HTTPError

def excuteApi(request, options):
	api_protocol = options.get("--api_protocol")
	m_ip = options.get("--m_ip")
	m_port = options.get("--m_port")
	secret_key = options.get("--secret_key")
	secretkey=secret_key

	baseurl=str(api_protocol)+'://'+str(m_ip)+':'+str(m_port)+'/client/api?'
	syslog.syslog(syslog.LOG_INFO, '222222=============================================')
	syslog.syslog(syslog.LOG_INFO, baseurl)
	request_str='&'.join(['='.join([k,urllib.parse.quote_plus(request[k])]) for k in request.keys()])
	sig_str='&'.join(['='.join([k.lower(),urllib.parse.quote_plus(request[k]).lower().replace('+','%20')])for k in sorted(request)])
	sig=hmac.new(secretkey.encode('utf-8'),sig_str.encode('utf-8'),hashlib.sha256)
	sig=hmac.new(secretkey.encode('utf-8'),sig_str.encode('utf-8'),hashlib.sha256).digest()
	sig=base64.encodebytes(hmac.new(secretkey.encode('utf-8'),sig_str.encode('utf-8'),hashlib.sha256).digest())
	sig=base64.encodebytes(hmac.new(secretkey.encode('utf-8'),sig_str.encode('utf-8'),hashlib.sha256).digest()).strip()
	sig=urllib.parse.quote_plus(base64.encodebytes(hmac.new(secretkey.encode('utf-8'),sig_str.encode('utf-8'),hashlib.sha256).digest()).strip())

	req=baseurl+request_str+'&signature='+sig
	context = ssl._create_unverified_context()
	res=urllib.request.urlopen(req, context=context)
	return res.read().decode()

def getVirtualMachinesStatus(options):
	# reqest 세팅
	request={}
	request['command']='listVirtualMachines'
	request['id']=options.get("--vm_id")
	request['response']='json'
	request['apikey']=options.get("--api_key")

	# API 호출
	syslog.syslog(syslog.LOG_INFO, 'collecting at getVirtualMachinesStatus')
	syslog.syslog(syslog.LOG_INFO, str(options))
	result = excuteApi(request, options)
	data = json.loads(result)
	state_value = data['listvirtualmachinesresponse']['virtualmachine'][0]['state']
	return str(state_value)

def setVirtualMachinesStop(options):
	# reqest 세팅
	request={}
	request['command']='stopVirtualMachine'
	request['id']=options.get("--vm_id")
	request['response']='json'
	request['apikey']=options.get("--api_key")

	# API 호출
	syslog.syslog(syslog.LOG_INFO, 'collecting at setVirtualMachinesStop')
	syslog.syslog(syslog.LOG_INFO, str(options))
	result = excuteApi(request, options)
	data = json.loads(result)
	state_value = data['stopvirtualmachineresponse']['jobid']
	return str(state_value)

def setVirtualMachinesStart(options):
	# reqest 세팅
	request={}
	request['command']='startVirtualMachine'
	request['id']=options.get("--vm_id")
	request['response']='json'
	request['apikey']=options.get("--api_key")

	# API 호출
	syslog.syslog(syslog.LOG_INFO, 'collecting at setVirtualMachinesStart')
	syslog.syslog(syslog.LOG_INFO, str(options))
	result = excuteApi(request, options)
	data = json.loads(result)
	state_value = data['startvirtualmachineresponse']['jobid']
	return str(state_value)

def get_power_status(_, options):
	state = getVirtualMachinesStatus(options)
	syslog.syslog(syslog.LOG_INFO, '1111=============================================')
	if state == "Running":
		return "on"
	elif state == "Stopped":
		return "off"
	else:
		return "unknown"


def set_power_status(_, options):
	try:
		if (options["--action"]=="off" or get_power_status(_, options) == "off"):
			syslog.syslog(syslog.LOG_INFO, 'VirtualMachinesStop=============================================')
			setVirtualMachinesStop(options)
		elif (options["--action"]=="on"):
			syslog.syslog(syslog.LOG_INFO, 'VirtualMachinesStart=============================================')
			setVirtualMachinesStart(options)
	except Exception as e:
		logging.debug("Failed to power %s %s: %s", \
					 options["--action"], options["--plug"], e)
		fail(EC_STATUS)


def reboot_cycle(_, options):
	(status, _, _) = run_command(options, create_command(options, "cycle"))
	return not bool(status)

def define_new_opts():
	all_opt["zone"] = {
		"getopt" : "z:",
		"longopt" : "zone",
		"help" : "-z, --zone=[zone]          Zone, e.g. zone1",
		"shortdesc" : "Zone.",
		"required" : "0",
		"order" : 2
	}
	all_opt["api_protocol"] = {
		"getopt" : "ap:",
		"longopt" : "api_protocol",
		"help" : "-ap, --api-protocol=[api-protocol]          Api protocol, e.g. http",
		"shortdesc" : "API Protocol.",
		"required" : "0",
		"order" : 3
	}
	all_opt["api_key"] = {
		"getopt" : "ak:",
		"longopt" : "api_key",
		"help" : "-a, --api-key=[key]         API Key",
		"shortdesc" : "API Key.",
		"required" : "0",
		"order" : 4
	}
	all_opt["secret_key"] = {
		"getopt" : "sk:",
		"longopt" : "secret_key",
		"help" : "-s, --secret_key=[key]         Secret Key",
		"shortdesc" : "Secret Key.",
		"required" : "0",
		"order" : 5
	}
	all_opt["vm_id"] = {
		"getopt" : "vmid:",
		"longopt" : "vm_id",
		"help" : "-vi, --vm-id=[option]      VM-ID",
		"shortdesc" : "VM ID.",
		"required" : "0",
		"order" : 6
	}
	all_opt["m_ip"] = {
		"getopt" : "mip:",
		"longopt" : "m_ip",
		"help" : "-mip, --mip=[mip]          MOLD Ip Address",
		"shortdesc" : "MOLD IP Address.",
		"required" : "0",
		"order" : 7
	}
	all_opt["m_port"] = {
		"getopt" : "mpt:",
		"longopt" : "m_port",
		"help" : "-mpt, --mport=[mport]      MOLD Port",
		"shortdesc" : "MOLD PORT.",
		"required" : "0",
		"order" : 8
	}

# Main agent method
def main():
	device_opt = ["port", "no_password", "zone", "api_protocol", "api_key", "secret_key", "vm_id", "m_ip", "m_port"]
	atexit.register(atexit_handler)

	define_new_opts()
	all_opt["power_timeout"]["default"] = "60"

	options = check_input(device_opt, process_input(device_opt))

	docs = {}
	docs["shortdesc"] = "Fence agent for MOLD"
	docs["longdesc"] = "fence_mold is a Power Fencing agent for MOLD\
\n.P\n\
"
	docs["vendorurl"] = "http://www.ablecloud.io"
	show_docs(options, docs)

	run_delay(options)

	# Operate the fencing device
	syslog.syslog(syslog.LOG_INFO, '**00000=============================================')
	result = fence_action(None, options, set_power_status, get_power_status, None)
	syslog.syslog(syslog.LOG_INFO, str(result))
	syslog.syslog(syslog.LOG_INFO, '**11111=============================================')
	sys.exit(result)

if __name__ == "__main__":
	main()
