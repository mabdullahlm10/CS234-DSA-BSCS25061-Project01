// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS // added due to some error on fopen

#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <fstream>
// #include <unistd.h>
// #include <sys/socket.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#endif

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
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {

        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH) { return; }
        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;

        top = newNode;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if (isEmpty()) { throw runtime_error("Stack is empty"); }
        Node* temp = top;
        T value = top->data;
        top = top->next;

        delete temp;
        count--;
        return value;
    }
    T &peek()
    {
        // returns the top value on the stack
        if (isEmpty()) { throw runtime_error("Stack is empty"); }

        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* current = top;
        int32_t copied = 0;

        while (current != nullptr && copied < maxLen)
        {
            out[copied] = current->data;
            current = current->next;
            copied++;
        }

        return copied;
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
        stepCount = 0;
    }

    void record(Snapshot *s)
    {
        // add record in the timeline
        TimelineNode* newNode = new TimelineNode;

        newNode->data = s;
        newNode->next = nullptr;
        newNode->prev = tail;

        if (head == nullptr)
        {
            head = newNode;
            tail = newNode;
        }
        else
        {
            tail->next = newNode;
            tail = newNode;
        }

        stepCount++;
    }

    TimelineNode *begin()
    {
        return head;
    }

    int32_t getStepCount()
    {
        return stepCount;
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
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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
    while (getline(in, out))
    {
        bool hasText = false;

        for (int i = 0; i < out.length(); i++)
        {
            if (out[i] != ' ' && out[i] != '\t' && out[i] != '\r')
            {
                hasText = true;
                break;
            }
        }

        if (hasText) { return true; }
    }

    return false;
}

string firstWord(const string &line)
{
    // returns first word from the input string
    int i = 0;
    
    // skips spaces till first word
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
    {
        i++;
    }

    string word = "";
    // read first word
    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
    {
        word += line[i];
        i++;
    }

    return word;
}

string secondWord(const string &line)
{
    // returns the second word
    int i = 0;

    // skips spaces till first word
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
    {
        i++;
    }

    // skip first word
    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
    {
        i++;
    }

    // skip spaces bw first and second
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
    {
        i++;
    }

    string word = "";
    
    // read second
    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
    {
        word += line[i];
        i++;
    }

    return word;
}

bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream source(sourcePath);

    if (!source.is_open())
    {
        cout << "Error: Could not open source file." << endl;
        return false;
    }

    Stack<string> functionStack;
    string line;

    while (readSourceLine(source, line))
    {
        string keyword = firstWord(line);

        if (keyword == "func")
        {
            string functionName = secondWord(line);

            // func must have a name
            if (functionName == "")
            {
                cout << "Error: Function name missing." << endl;
                return false;
            }

            // nested functions are not allowed
            if (!functionStack.isEmpty())
            {
                cout << "Error: Nested function found." << endl;
                return false;
            }

            functionStack.push(functionName);
        }

        else if (keyword == "func_end")
        {
            // no func exists to match this func_end
            if (functionStack.isEmpty())
            {
                cout << "Error: func_end without matching func." << endl;
                return false;
            }

            functionStack.pop();
        }
    }

    // a func was opened but never closed
    if (!functionStack.isEmpty())
    {
        cout << "Error: func without matching func_end." << endl;
        return false;
    }

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t recordStart = ftell(f);
    int32_t stringSize = text.length();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&stringSize, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, stringSize, f);

    return recordStart;
}

int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t offsetField;
    int32_t stringSize;

    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }

    if (fread(&stringSize, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }

    outText = "";
    for (int i = 0; i < stringSize; i++)
    {
        char ch;
        if (fread(&ch, sizeof(char), 1, f) != 1)
        {
            return -1;
        }

        outText += ch;
    }

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

    ifstream source(sourcePath);
    if (!source.is_open())
    {
        cout << "Error: Could not open source file." << endl;
        return -1;
    }

    FILE* resolveFile = fopen(resolveBinPath, "wb+");

    if (resolveFile == nullptr)
    {
        cout << "Error: Could not create resolve.bin." << endl;
        return -1;
    }

    string line;
    int64_t mainOffset = -1;

    // write every line into resolve.bin
    while (readSourceLine(source, line))
    {
        int64_t recordStart = ftell(resolveFile);
        writeResolveRecord(resolveFile, recordStart, line);
        string keyword = firstWord(line);

if (keyword == "func")
{
    string functionName = secondWord(line);

    if (funcCount >= MAX_FUNCS)
    {
        cout << "Error: Too many functions." << endl;
        fclose(resolveFile);
        return -1;
    }

    funcArray[funcCount].funcName = functionName;
    funcArray[funcCount].byteOffsetInResolveBin = recordStart;
    funcCount++;

    if (functionName == "main")
    {
        mainOffset = recordStart;
    }
}
else if (keyword == "call")
{
    if (patchCount >= MAX_PATCHES)
    {
        cout << "Error: Too many function calls." << endl;
        fclose(resolveFile);
        return -1;
    }

    patches[patchCount].byteOffsetOfOffsetField = recordStart;
    patches[patchCount].targetFuncName = secondWord(line);
    patchCount++;
}
    }

    // main should exist
    if (mainOffset == -1)
    {
        cout << "Error: main function not found." << endl;
        fclose(resolveFile);
        return -1;
    }

    // fix all call offset
    for (int i = 0; i < patchCount; i++)
    {
        int64_t targetOffset = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (patches[i].targetFuncName == funcArray[j].funcName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset == -1)
        {
            cout << "Error: Undefined function " << patches[i].targetFuncName << endl;
            fclose(resolveFile);
            return -1;
        }

        fseek(resolveFile, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, resolveFile);
    }

    fclose(resolveFile);
    return mainOffset;
}

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

Variable* findVariable(Frame& frame, const string& name)
{
    // check arguments
    for (int i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
        {
            return &frame.argv[i];
        }
    }

    // check local variables
    for (int i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            return &frame.locals[i];
        }
    }

    return nullptr;
}

bool isNumber(const string& text)
{
    if (text == "") { return false; }

    int i = 0;
    if (text[0] == '-')
    {
        i = 1;
    }

    if (i >= text.length())
    {
        return false;
    }

    while (i < text.length())
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return false;
        }

        i++;
    }

    return true;
}

bool getValue(Frame& frame, const string& text, int32_t& value)
{
    if (isNumber(text))
    {
        value = stoi(text);
        return true;
    }

    Variable* var = findVariable(frame, text);
    if (var == nullptr) { return false; }

    value = var->value;
    return true;
}

bool setVariable(Frame& frame, const string& name, int32_t value)
{
    Variable* var = findVariable(frame, name);

    if (var != nullptr)
    {
        var->value = value;
        return true;
    }

    if (frame.localCount >= MAX_VARS_PER_FRAME)
    {
        return false;
    }

    frame.locals[frame.localCount].name = name;
    frame.locals[frame.localCount].value = value;
    frame.localCount++;

    return true;
}

int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated

    int i = 0;
    int32_t count = 0;

    while (i < line.length() && count < maxTokens)
    {
        // skip spaces
        while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
        {
            i++;
        }

        if (i >= line.length())
        {
            break;
        }

        string word = "";
        // read one word
        while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
        {
            word += line[i];
            i++;
        }

        tokens[count].text = word;
        if (count == 0)
        {
            tokens[count].type = KEYWORD;
        }
        else if (count == 1)
        {
            tokens[count].type = IDENTIFIER;
        }
        else
        {
            tokens[count].type = PARAM;
        }

        count++;
    }

    return count;
}

Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
    Snapshot* snapshot = new Snapshot;

    snapshot->stackDepth = callStack.snapshot_into(snapshot->callStack, MAX_STACK_DEPTH);

    return snapshot;
}

void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
    if (mainOffset < 0)
    {
        cout << "Error: Invalid main offset." << endl;
        return;
    }

    FILE* resolveFile = fopen(resolveBinPath, "rb");

    if (resolveFile == nullptr)
    {
        cout << "Error: Could not open resolve.bin." << endl;
        return;
    }

    Stack<Frame> callStack;

    // create Main frame
    Frame mainFrame{};
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    // execution starts from main
    fseek(resolveFile, mainOffset, SEEK_SET);

    while (!callStack.isEmpty())
    {
        int64_t recordStart = ftell(resolveFile);
        string line;
        int64_t offsetField = readResolveRecord(resolveFile, line);

        if (offsetField == -1)
        {
            cout << "Error: Could not read instruction." << endl;
            break;
        }

        Token tokens[MAX_TOKENS];
        int32_t tokenCount = tokenizeLine(line, tokens, MAX_TOKENS);

        if (tokenCount == 0)
        {
            continue;
        }

        string keyword = tokens[0].text;

        //Func does not perform an operation itseld
        if (keyword == "func")
        {
        }

        // SET
        else if (keyword == "set")
        {
            if (tokenCount < 3)
            {
                cout << "Error: Invalid set Instruction." << endl;
                break;
            }

            Frame& currentFrame = callStack.peek();
            int32_t value;

            if (!getValue(currentFrame, tokens[2].text, value))
            {
                cout << "Error: Invalid value in set." << endl;
                break;
            }

            if (!setVariable(currentFrame, tokens[1].text, value))
            {
                cout << "Error: Could not set variable." << endl;
                break;
            }
        }

        // ADD, SUB, MUL, DIV
        else if (keyword == "add" || keyword == "sub" || keyword == "mul" || keyword == "div")
        {
            if (tokenCount < 3)
            {
                cout << "Error: Invalid arithmetic instruction" << endl;
                break;
            }

            Frame& currentFrame = callStack.peek();
            Variable* first = findVariable(currentFrame, tokens[1].text);

            if (first == nullptr)
            {
                cout << "Error: Variable not found." << endl;
                break;
            }

            int32_t secondValue;
            if (!getValue(currentFrame, tokens[2].text, secondValue))
            {
                cout << "Error: Variable not found." << endl;
                break;
            }

            if (keyword == "add")
            {
                first->value = first->value + secondValue;
            }
            else if (keyword == "sub")
            {
                first->value = first->value - secondValue;
            }
            else if (keyword == "mul")
            {
                first->value = first->value * secondValue;
            }
            else if (keyword == "div")
            {
                if (secondValue == 0)
                {
                    cout << "Error: Division by zero." << endl;
                    break;
                }

                first->value = first->value / secondValue;
            }
        }
        else if (keyword == "call")
        {
            int64_t targetOffset = offsetField;

            // temp read called functions FUNC Line
            fseek(resolveFile, targetOffset, SEEK_SET);
            string funcLine;
            readResolveRecord(resolveFile, funcLine);
            Token funcTokens[MAX_TOKENS];
            int32_t funcTokenCount = tokenizeLine(funcLine, funcTokens, MAX_TOKENS);

            if (funcTokenCount < 2 || funcTokens[0].text != "func")
            {
                cout << "Error: Invalid function terget." << endl;
                break;
            }

            int actualCount = tokenCount - 2;
            int parameterCount = funcTokenCount - 2;

            if (actualCount != parameterCount)
            {
                cout << "Error: Argumrnt count does not match." << endl;
                break;
            }

            Frame newFrame{};
            newFrame.func_name = funcTokens[1].text;
            newFrame.argc = parameterCount;
            newFrame.returnLine = recordStart;
            newFrame.localCount = 0;
            Frame& caller = callStack.peek();

            for (int i = 0; i < parameterCount; i++)
            {
                Variable* actualVariable =
                    findVariable(caller, tokens[i + 2].text);

                if (actualVariable == nullptr)
                {
                    cout << "Error: Call argument not found." << endl;
                    fclose(resolveFile);
                    return;
                }

                newFrame.argv[i].name = funcTokens[i + 2].text;
                newFrame.argv[i].value = actualVariable->value;
            }

            callStack.push(newFrame);

            // next instruction should be called function's FUNC header
            fseek(resolveFile, targetOffset, SEEK_SET);
        }
        // FUNCTION END / RETURN
        else if (keyword == "func_end")
        {
            Frame finishedFrame = callStack.pop();

            // ending main mean program finished
            if (callStack.isEmpty())
            {
                timeline.record(buildSnapshot(callStack));
                break;
            }

            // go back to Call instruction
            fseek(resolveFile, finishedFrame.returnLine, SEEK_SET);
            string callLine;
            readResolveRecord(resolveFile, callLine);
            Token callTokens[MAX_TOKENS];
            int32_t callTokenCount = tokenizeLine(callLine, callTokens, MAX_TOKENS);
            Frame& caller = callStack.peek();

            // copy changed arguments back into caller
            for (int i = 0; i < finishedFrame.argc; i++)
            {
                if (i + 2 < callTokenCount)
                {
                    setVariable(
                        caller,
                        callTokens[i + 2].text,
                        finishedFrame.argv[i].value
                    );
                }
            }
        }
        // snapshot after every executed line
        timeline.record(buildSnapshot(callStack));
    }
    fclose(resolveFile);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
    FILE* file = fopen(tdbgPath, "wb+");

    if (file == nullptr)
    {
        cout << "Error: Could not create .tdbg file." << endl;
        return;
    }

    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';

    header.version = 1;
    header.stepCount = timeline.getStepCount();
    header.indexOffset = 0;
    // first wtrite header with temp indecx offset
    writeHeader(file, header);
    vector<int64_t> index;
    TimelineNode* current = timeline.begin();

    // write every snapshot
    while (current != nullptr)
    {
        int64_t snapshotOffset = ftell(file);
        index.push_back(snapshotOffset);
        fwrite(current->data, sizeof(Snapshot), 1, file);
        current = current->next;
    }

    // index start heere
    header.indexOffset = ftell(file);

    // wirte dense index
    for (int i = 0; i < index.size(); i++)
    {
        fwrite(&index[i], sizeof(int64_t), 1, file);
    }

    // go back and update header
    fseek(file, 0, SEEK_SET);
    writeHeader(file, header);
    fclose(file);
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

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}

