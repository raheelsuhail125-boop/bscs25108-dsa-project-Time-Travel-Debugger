
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        count =0;
        top=nullptr;
    }
    void push(const T &val)
    {

        // pushes the value on the stack if max limit is not reached yet.
        if (count == MAX_STACK_DEPTH){
            cout<<"Error"<<endl;
            return;
        }
        Node*n=new Node;
        n->data=val;
        n->next=top;
        top=n;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if(top==nullptr){
            cout<<"Empty";
            return T();
        }
    }
    T &peek()
    {
        // returns the top value on the stack
        if(top==nullptr){
            cout<<"Empty";
            return T();
        }
        return top->data;

    }
    bool isEmpty()
    {
        return count==0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t ct=0;
        Node*frame=top;
        while(frame!=nullptr){
            out[ct]=frame->data;
            frame=frame->next;
            ct++;
        }
        return ct;

    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        ct= 0;
    }
    void record(Snapshot *s)
    {
        TimelineNode *n = new TimelineNode;
        n->data = s;
        n->next = nullptr;
        n->prev = tail;
        if (tail == nullptr)
            head = n;
        else
            tail->next = n;
        tail = n;
        ct++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {   
        return ct;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    // reads the next nonblank line
    string line;
    while(getline(in,line)){
        if(line!="")
        {
            out =line;
            return true;
        }
    }
    return false;
}
string firstWord(const string &line)
{
    // returns first word from the input string
    string word="";
    int i =0;
    int n =line.size();
    while(i<n&&line[i]==' ')
        i++;
     while(i<n&&line[i]!=' '){
        word+=line[i];
        i++;
    }
    return word;

}
string secondWord(const string &line)
{
    // returns the second word
    string word="";
    int i =0;
    int n =line.size();
    while(i<n&&line[i]==' ')
        i++;
    while(i<n&&line[i]!=' ')
        i++;
    while(i<n&&line[i]==' ')
        i++;
    while(i<n&&line[i]!=' '){
        word+=line[i];
        i++;
    }
    return word;
}
bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream in(sourcePath);
    if(!in){
        cout<<"Error File no Open"<<endl;
        return false;}
    string line;
    bool incheck=false;
    while(readSourceLine(in,line)){
        string word=firstWord(line);
        if(word=="func"){
            if(incheck==true){
                cout<<"Error Nested Function"<<endl;
                return false;
            }
            incheck=true;
        }
        if(word=="func_end"){
            if (incheck==false){
                cout<<"Error func_end is without func in start"<<endl;
                return false;
            }
            incheck=false;
        }
    }
    if(incheck==true){
        cout<<"Error func never closed"<<endl;
        return false;
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t start = ftell(f);
    int32_t size = text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, size, f);
    return start;
    
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t offsetField;
    int32_t size;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
        return -1;
    string text(size, ' ');
    if (size > 0 && fread(&text[0], 1, size, f) != (size_t)size)
        return -1;
    outText = text;
    return offsetField;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
    ifstream in(sourcePath);
    if(!in){
        cout<<"Error "<<sourcePath<<endl;
        return -1;
    }
    FILE *f=fopen(resolveBinPath,"w+b");
    if(f==NULL){
        cout<<"Cannot create "<<resolveBinPath<<endl;
        return -1;
    }
    int64_t offset =0;
    string line;
    while(readSourceLine(in,line)){
        string word=firstWord(line);
        if(word=="call"){
            patches[patchCount].byteOffsetOfOffsetField=offset;
            patches[patchCount].targetFuncName=secondWord(line);
            patchCount++;
            writeResolveRecord(f,0,line);
        }
        else
        {
            if(word=="func"){
                funcArray[funcCount].funcName=secondWord(line);
                funcArray[funcCount].byteOffsetInResolveBin=offset;
                funcCount++;
            }
            writeResolveRecord(f,offset,line);}
            offset=offset+8+4+line.size();
        
    }

     for (int i = 0; i < patchCount; i++)
    {
        int64_t target = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
                target = funcArray[j].byteOffsetInResolveBin;
        }
        if (target == -1)
        {
            cout << "Error undefined function " << patches[i].targetFuncName << endl;
            fclose(f);
            return -1;
        }
        fseek(f, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, f);
    }
    for (int j = 0; j < funcCount; j++)
    {
        if (funcArray[j].funcName == "main")
            return funcArray[j].byteOffsetInResolveBin;
    }
    cout << "Error no main" << endl;
    return -1;}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t count = 0;
    size_t i = 0;
    while (i < line.size() && count < maxTokens)
    {
        while (i < line.size() && line[i] == ' ')
            i++;
        if (i >= line.size())
            break;

        string word = "";
        while (i < line.size() && line[i] != ' ')
        {
            word += line[i];
            i++;
        }
        if (count == 0)
            tokens[count].type = KEYWORD;
        else if (count == 1)
            tokens[count].type = IDENTIFIER;
        else
            tokens[count].type = PARAM;
        tokens[count].text = word;
        count++;
    }
    return count;
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
    Snapshot *s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}
bool isNumber(const string &s)
{
    if (s.size() == 0)
        return false;
    size_t i = 0;
    if (s[0] == '-' && s.size() > 1)
        i = 1;
    for (; i < s.size(); i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

Variable *findVar(Frame &fr, const string &name)
{
    for (int i = 0; i < fr.argc; i++)
    {
        if (fr.argv[i].name == name)
            return &fr.argv[i];
    }
    for (int i = 0; i < fr.localCount; i++)
    {
        if (fr.locals[i].name == name)
            return &fr.locals[i];
    }
    return nullptr;
}

bool getValue(Frame &fr, const string &text, int32_t &out)
{
    if (isNumber(text))
    {
        out = stoi(text);
        return true;
    }
    Variable *v = findVar(fr, text);
    if (v == nullptr)
        return false;
    out = v->value;
    return true;
}
bool doSet(Frame &fr, Token tokens[], int32_t n)
{
    if (n < 3)
    {
        cout << "Error set needs a name and a value" << endl;
        return false;
    }
    int32_t x;
    if (!getValue(fr, tokens[2].text, x))
    {
        cout << "Error unknown variable " << tokens[2].text << endl;
        return false;
    }
    Variable *v = findVar(fr, tokens[1].text);
    if (v == nullptr)
    {
        if (fr.localCount >= MAX_VARS_PER_FRAME)
        {
            cout << "Error Too many variables" << endl;
            return false;
        }
        v = &fr.locals[fr.localCount];
        v->name = tokens[1].text;
        fr.localCount++;
    }
    v->value = x;
    return true;
}

bool doMath(Frame &fr, Token tokens[], int32_t n)
{
    if (n < 3)
    {
        cout << "Error: " << tokens[0].text << " needs two operands" << endl;
        return false;
    }
    Variable *target = findVar(fr, tokens[1].text);
    if (target == nullptr)
    {
        cout << "Unknown variable " << tokens[1].text << endl;
        return false;
    }
    int32_t x;
    if (!getValue(fr, tokens[2].text, x))
    {
        cout << "Unknown variable " << tokens[2].text << endl;
        return false;
    }
    string op = tokens[0].text;
    if (op == "add")
        target->value = target->value + x;
    else if (op == "sub")
        target->value = target->value - x;
    else if (op == "mul")
        target->value = target->value * x;
    else if (op == "div")
    {
        if (x == 0)
        {
            cout << "Error divide by zero" << endl;
            return false;
        }
        target->value = target->value / x;
    }
    return true;
}
bool doCall(FILE *f, Stack<Frame> &callStack, Token tokens[], int32_t n, int64_t target, int64_t lineStart)
{
    if (callStack.depth() >= MAX_STACK_DEPTH)
    {
        cout << "Error: stack overflow" << endl;
        return false;
    }
    Frame &caller = callStack.peek();
    int32_t argCount = n - 2;

    fseek(f, target, SEEK_SET);
    string header;
    readResolveRecord(f, header);
    Token ht[MAX_TOKENS];
    int32_t hn = tokenizeLine(header, ht, MAX_TOKENS);
    int32_t paramCount = hn - 2;
    if (argCount != paramCount)
    {
        cout << "Error wrong number of arguments for " << ht[1].text << endl;
        return false;
    }

    Frame fresh = Frame();
    fresh.func_name = ht[1].text;
    fresh.argc = paramCount;
    fresh.returnLine = (int32_t)lineStart;
    fresh.localCount = 0;
    for (int i = 0; i < paramCount; i++)
    {
        int32_t x;
        if (!getValue(caller, tokens[2 + i].text, x))
        {
            cout << "Error unknown variable " << tokens[2 + i].text << endl;
            return false;
        }
        fresh.argv[i].name = ht[2 + i].text;
        fresh.argv[i].value = x;
    }
    callStack.push(fresh);
    return true;
}

void doReturn(FILE *f, Stack<Frame> &callStack)
{
    Frame done = callStack.pop();
    if (callStack.isEmpty())
        return;

    fseek(f, done.returnLine, SEEK_SET);
    string callLine;
    readResolveRecord(f, callLine);
    Token ct[MAX_TOKENS];
    tokenizeLine(callLine, ct, MAX_TOKENS);

    Frame &caller = callStack.peek();
    for (int i = 0; i < done.argc; i++)
    {
        string name = ct[2 + i].text;
        if (!isNumber(name))
        {
            Variable *v = findVar(caller, name);
            if (v != nullptr)
                v->value = done.argv[i].value;
        }
    }
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack
    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {

        // send an error response instead of a .tdbg file
        return 1;
    }
   
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
     cout << "mainOffset = " << mainOffset << endl;
    return 0;

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}