#include "beascript.h"
#include <sstream>
#include <iostream>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>

using namespace v8;
namespace bea{
	
	logCallback BeaContext::m_logger = NULL; 
	yieldCallback BeaContext::m_yielder = NULL;

	std::string Global::scriptDir = std::string();
	reportExceptionCb Global::reportException = _BeaScript::reportError; 
	Persistent<ObjectTemplate> Global::externalTemplate;
	

	// Reads a file into a v8 string.
	v8::Handle<v8::String> ReadFile(const char* name) {
		FILE* file = fopen(name, "rb");
		if (file == NULL) return v8::Handle<v8::String>();

		fseek(file, 0, SEEK_END);
		int size = ftell(file);
		rewind(file);

		char* chars = new char[size + 1];
		chars[size] = '\0';
		for (int i = 0; i < size;) {
			int read = (int) fread(&chars[i], 1, size - i, file);
			i += read;
		}
		fclose(file);
		v8::Handle<v8::String> result = v8::String::New(chars, size);
		delete[] chars;
		return result;
	}

	//Logs a message to the console
	static Handle<Value> Log(const Arguments& args) {
		if (args.Length() < 1) return v8::Undefined();
		HandleScope scope;
		Handle<Value> arg = args[0];
		v8::String::Utf8Value value(arg);
		//printf("Logged: %s\n", *value);
		std::cout << "Logged: " << *value << std::endl;
		return v8::Undefined();
	}

	//////////////////////////////////////////////////////////////////////////

	std::string BeaContext::lastError; 
	boost::filesystem::path _BeaScript::scriptPath;

	

	//Include a script file into current context
	//Raise javascript exception if load failed 
	Handle<Value> _BeaScript::include( const Arguments& args )
	{
		boost::filesystem::path parentPath = scriptPath.parent_path();

		HandleScope scope; 
		Handle<Value> result;
		
		//v8::String::Utf8Value fileName(args[i]);
		std::string fileName = bea::Convert<std::string>::FromJS(args[0], 0);

		//Add the script path to it
		boost::filesystem::path absolutePath = parentPath / fileName; 

		Handle<v8::String> source = ReadFile(absolutePath.string().c_str());

		if (source.IsEmpty()){
			std::stringstream s;
			s << "Could not include file " << fileName.c_str();
			return v8::ThrowException(v8::Exception::Error(v8::String::New(s.str().c_str())));
		}

		result = execute(source);
			
		if (result.IsEmpty())
			return v8::ThrowException(v8::Exception::Error(v8::String::New(lastError.c_str())));
	
		return scope.Close(result);
	}
	
	//Execute a string of script
	Handle<Value> _BeaScript::execute( Handle<v8::String> script )
	{
		HandleScope scope;
		TryCatch try_catch;
		Handle<Value> result; 

		// Compile the script and check for errors.
		Handle<v8::Script> compiled_script = v8::Script::Compile(script);
		if (compiled_script.IsEmpty()) {
			reportError(try_catch);
			return result;
		}

		// Run the script!
		result = compiled_script->Run();

		if (result.IsEmpty()) {
			reportError(try_catch);
			return result;
		}

		return scope.Close(result);
	}

	//Report the error from an exception, store it in lastError
	void BeaContext::reportError(TryCatch& try_catch){

		v8::String::Utf8Value error(try_catch.Exception());
		std::stringstream strstr;
		strstr << try_catch.Message()->GetLineNumber() << ": " << *error;
		lastError = strstr.str();

		if (m_logger)
			m_logger(lastError.c_str());

	}

	//Initialize the javascript context and load a script file into it
	bool _BeaScript::loadScript( const char* fileName )
	{
		v8::Locker locker; 
		if (!init())
			return false; 

		scriptPath = boost::filesystem::system_complete(fileName);

		Global::scriptDir = scriptPath.parent_path().string();

		Context::Scope context_scope(m_context);

		HandleScope scope;
		Handle<v8::String> str = ReadFile(fileName);

		if (str.IsEmpty())
			return false; 

		Handle<Value> v = execute(str);

		return !v.IsEmpty();
	}

	//Initialize the javascript context and expose the methods in exposer
	bool _BeaScript::init()
	{

		lastError = "";
		HandleScope handle_scope;
		Handle<ObjectTemplate> global = ObjectTemplate::New();

		//Create the context
		m_context = Context::New(NULL, global);

		Context::Scope context_scope(m_context);

		Global::InitExternalTemplate();

		BEA_SET_METHOD(m_context->Global(), "require", include);
		BEA_SET_METHOD(m_context->Global(), "log", Log);
		BEA_SET_METHOD(m_context->Global(), "yield", yield);
		BEA_SET_METHOD(m_context->Global(), "collectGarbage", collectGarbage);

		expose();
		return true; 
	}

	v8::Handle<v8::Value> _BeaScript::collectGarbage( const v8::Arguments& args ){

		while (!V8::IdleNotification()) {}
		return args.This();
	}

	v8::Handle<v8::Value> _BeaScript::yield( const v8::Arguments& args )
	{
		{
			int timeToYield = bea::Optional<int>::FromJS(args, 0, 10); 

			v8::Unlocker unlocker;

			if (m_yielder)
				m_yielder(timeToYield);

			//Cleanup garbage
			//while (!V8::IdleNotification()) {}		

		}
		return args.This();
	}
	//Call a javascript function, store the found function in a local cache for faster access
	Handle<Value> BeaContext::call(const char *fnName, int argc, Handle<Value> argv[]){
		
		HandleScope scope;
		Context::Scope context_scope(m_context);

		//Lookup function in cache
		JFunction fn;
		CacheMap::iterator iter = m_fnCached.find(std::string(fnName));

		if (iter != m_fnCached.end())
			fn = iter->second;
		else {
			//Lookup function in the script 
			Handle<Value> fnv = m_context->Global()->Get(v8::String::New(fnName));

			if (!fnv->IsFunction()) {
				std::stringstream strstr;
				strstr << "Error: " << fnName << " is not a function";
				lastError = strstr.str();
				return v8::False();
			} else {

				//Store found function in our cache
				fn = Persistent<Function>::New(Handle<Function>::Cast(fnv));
				m_fnCached[std::string(fnName)] = fn;
			}
		}

		//Call the function
		TryCatch try_catch;
		Handle<Value> result = fn->Call(m_context->Global(), argc, argv);

		if (result.IsEmpty())
			reportError(try_catch);

		return scope.Close(result);
	}


	BeaContext::BeaContext()
	{

	}

	BeaContext::~BeaContext()
	{
		for (CacheMap::iterator iter = m_fnCached.begin(); iter!= m_fnCached.end(); iter++){
			iter->second.Dispose();
		}

		m_fnCached.empty();
		m_context.Dispose();
	}

	bool BeaContext::exposeGlobal( const char* name, v8::InvocationCallback cb )
	{
		return BEA_SET_METHOD(m_context->Global(), name, cb);
	}

	bool BeaContext::exposeToObject( const char* targetName, const char* exposedName, v8::Handle<v8::Value> what )
	{
		HandleScope scope; 
		Handle<Value> jc = m_context->Global()->Get(v8::String::New(targetName));
		if (!jc.IsEmpty() && jc->IsObject()){
			Handle<Object> obj = jc->ToObject();
			return obj->Set(v8::String::New(exposedName), what);
		}
		return false; 
	}
}	//namespace bea