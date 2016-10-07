/*********************************************************************
 *   Copyright 2016, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *********************************************************************/

#include "d4includes.h"
#include <stdarg.h>
#include "ezxml.h"

/**
 * Build the netcdf-4 metadata from the NCD4node nodes.
 */

/***************************************************/
/* Forwards */

static char* backslashEscape(const char* s);
static char* getFieldFQN(NCD4node* field, const char* tail);
static int build(NCD4meta* builder, NCD4node* root);
static int buildAtomicVar(NCD4meta* builder, NCD4node* var);
static int buildAttributes(NCD4meta* builder, NCD4node* varorgroup);
static int buildCompound(NCD4meta* builder, NCD4node* cmpdtype, NCD4node* group, char* name);
static int buildDimension(NCD4meta* builder, NCD4node* dim);
static int buildEnumeration(NCD4meta* builder, NCD4node* en);
static int buildGroups(NCD4meta*, NCD4node* parent);
static int buildMaps(NCD4meta* builder, NCD4node* var);
static int buildMetaData(NCD4meta* builder, NCD4node* var);
static int buildOpaque(NCD4meta* builder, NCD4node* op);
static int buildSequence(NCD4meta* builder, NCD4node* seq);
static int buildSequenceType(NCD4meta* builder, NCD4node* seqtype);
static int buildStructure(NCD4meta* builder, NCD4node* structvar);
static int buildStructureType(NCD4meta* builder, NCD4node* structtype);
static int buildVariable(NCD4meta* builder, NCD4node* var);
static int compileAttrValues(NCD4meta* builder, NCD4node* basetype, NClist* values, void** memoryp);
static void computeOffsets(NCD4meta* builder, NCD4node* cmpd);
static unsigned long long computeTypeSize(NCD4meta* builder, NCD4node* type);
static int convertString(union ATOMICS* converter, NCD4node* type, const char* s);
static void* copyAtomic(union ATOMICS* converter, nc_type type, size_t len, void* dst);
static int decodeEconst(NCD4meta* builder, NCD4node* enumtype, const char* nameorval, union ATOMICS* converter);
static int downConvert(union ATOMICS* converter, NCD4node* type);
static void freeStringMemory(char** mem, int count);
static size_t getDimrefs(NCD4node* var, int* dimids);
static size_t getDimsizes(NCD4node* var, size_t* dimsizes);
static NCD4node* groupFor(NCD4node* node);
static void reclaimNode(NCD4node* node);

/***************************************************/
/* API */

int
NCD4_metabuild(NCD4meta* metadata, int ncid)
{
    int ret = NC_NOERR;
    int i;

    metadata->ncid = ncid;
    metadata->root->meta.id = ncid;
    /* Fix up the atomic types */
    for(i=0;i<nclistlength(metadata->allnodes);i++) {
	NCD4node* n = (NCD4node*)nclistget(metadata->allnodes,i);
	if(n->sort != NCD4_TYPE) continue;
	if(n->subsort > NC_MAX_ATOMIC_TYPE) continue;
	n->meta.id = n->subsort;
    }
    /* Topo sort the set of all nodes */
    NCD4_toposort(metadata);
    /* Process the metadata state */
    ret = build(metadata,metadata->root);
    return ret;
}


/* Create an empty NCD4meta object for
   use in subsequent calls
   (is the the right src file to hold this?)
*/

NCD4meta*
NCD4_newmeta(NCD4mode checksummode, size_t rawsize, void* rawdata)
{
    NCD4meta* meta = (NCD4meta*)calloc(1,sizeof(NCD4meta));
    if(meta == NULL) return NULL;
    meta->allnodes = nclistnew();
    meta->checksummode = checksummode;
    meta->serial.rawsize = rawsize;
    meta->serial.rawdata = rawdata;
#ifdef D4DEBUG
    meta->debuglevel = 1;
#endif
    return meta;
}

void
NCD4_setdebuglevel(NCD4meta* meta, int debuglevel)
{
    meta->debuglevel = debuglevel;
}

void
NCD4_reclaimMeta(NCD4meta* dataset)
{
    int i;
    if(dataset == NULL) return;
    for(i=0;i<nclistlength(dataset->allnodes);i++) {
	NCD4node* node = (NCD4node*)nclistget(dataset->allnodes,i);
	reclaimNode(node);
    } 
    nullfree(dataset->error.parseerror);
    nullfree(dataset->error.message);
    nullfree(dataset->error.context);
    nullfree(dataset->error.otherinfo);
    nullfree(dataset->serial.errdata);
    for(i=0;i<nclistlength(dataset->blobs);i++) {
	void* p = nclistget(dataset->blobs,i);
	nullfree(p);
    }
    nclistfree(dataset->blobs);
    free(dataset);
}

static void
reclaimNode(NCD4node* node)
{
    nullfree(node->name);
    nullfree(node->group.dapversion);
    nullfree(node->group.dmrversion);
    nullfree(node->group.datasetname);
    nclistfree(node->group.elements);
    nclistfree(node->en.econsts);
    nclistfreeall(node->attr.values);
    nclistfree(node->groups);
    nclistfree(node->vars);
    nclistfree(node->types);
    nclistfree(node->dims);
    nclistfree(node->attributes);
    nclistfree(node->maps);
}

/**************************************************/

/* Recursively walk the tree to create the metadata */
static int
build(NCD4meta* builder, NCD4node* root)
{
    int i,ret = NC_NOERR;
    size_t len = nclistlength(builder->allnodes);
    /* Start by defining  group tree separately so we can maintain
       order */
    if((ret=buildGroups(builder,root))) goto done;
    for(i=0;i<len;i++) {/* Walk in postfix order */
	NCD4node* x = (NCD4node*)nclistget(builder->allnodes,i);
	switch (x->sort) {
	case NCD4_DIM: if((ret=buildDimension(builder,x))) goto done; break;
	case NCD4_TYPE:
	    switch (x->subsort) {
	    case NC_ENUM: if((ret=buildEnumeration(builder,x))) goto done; break;
	    case NC_OPAQUE: if((ret=buildOpaque(builder,x))) goto done; break;
	    case NC_STRUCT: if((ret=buildStructureType(builder,x))) goto done; break;
	    case NC_SEQ: if((ret=buildSequenceType(builder,x))) goto done; break;
	    default: break;
	    }
	    break;
	default: break;
	}
    }
    /* Finally, define the top-level variables */
    for(i=0;i<len;i++) {
	NCD4node* x = (NCD4node*)nclistget(builder->allnodes,i);
	if(ISVAR(x->sort) && ISTOPLEVEL(x)) buildVariable(builder,x);
    }
done:
    return ret;
}

static int
buildGroups(NCD4meta* builder, NCD4node* parent)
{
    int i,ret=NC_NOERR;
#ifdef D4DEBUG
    fprintf(stderr,"build group: %s\n",parent->name);
#endif
    for(i=0;i<nclistlength(parent->groups);i++) {
	NCD4node* g = (NCD4node*)nclistget(parent->groups,i);
        if(g->group.isdataset) {
	    g->meta.id = builder->ncid;
        } else {
	    NCCHECK((nc_def_grp(parent->meta.id,g->name,&g->meta.id)));
        }
	if((ret=buildGroups(builder,g))) goto done; /* recurse */
    }
done:
    return ret;
}

static int
buildDimension(NCD4meta* builder, NCD4node* dim)
{
    int ret = NC_NOERR;
    NCD4node* group = groupFor(dim);
    NCCHECK((nc_def_dim(group->meta.id,dim->name,(size_t)dim->dim.size,&dim->meta.id)));
done:
    return ret;
}

static int
buildEnumeration(NCD4meta* builder, NCD4node* en)
{
    int i,ret = NC_NOERR;
    NCD4node* group = groupFor(en);
    NCCHECK((nc_def_enum(group->meta.id,en->basetype->meta.id,en->name,&en->meta.id)));
    for(i=0;i<nclistlength(en->en.econsts);i++) {	
	NCD4node* ec = (NCD4node*)nclistget(en->en.econsts,i);
	NCCHECK((nc_insert_enum(group->meta.id, en->meta.id, ec->name, ec->en.ecvalue.i8)));
    }
done:
    return ret;
}

static int
buildOpaque(NCD4meta* builder, NCD4node* op)
{
    int ret = NC_NOERR;
    NCD4node* group = groupFor(op);

    /* Two cases: fixed size and true varying size */
    if(op->opaque.size > 0) {
	char* name  = op->name;
	/* Again, two cases, with and without UCARTAGORIGTYPE */
	if(op->nc4.orig.name != NULL) {
	    name = op->nc4.orig.name;
	    group = op->nc4.orig.group;		
	}
	NCCHECK((nc_def_opaque(group->meta.id,op->opaque.size,name,&op->meta.id)));
    } else {
	/* create in root as ubyte(*) vlen named "_bytestring" */
	NCCHECK((nc_def_vlen(builder->root->meta.id,"_bytestring",NC_UBYTE,&op->meta.id)));
    }
done:
    return ret;
}

static int
buildVariable(NCD4meta* builder, NCD4node* var)
{
    int ret = NC_NOERR;

    switch (var->subsort) {
    default:
	if((ret = buildAtomicVar(builder,var))) goto done;
	break;
    case NC_STRUCT:
	if((ret = buildStructure(builder,var))) goto done;
	break;
    case NC_SEQ:
	if((ret = buildSequence(builder,var))) goto done;
	break;
    }
done:
    return ret;
}

static int
buildMetaData(NCD4meta* builder, NCD4node* var)
{
    int ret = NC_NOERR;
    if((ret = buildAttributes(builder,var))) goto done;    
    if((ret = buildMaps(builder,var))) goto done;    
done:
    return ret;
}

static int
buildMaps(NCD4meta* builder, NCD4node* var)
{
    int i,ret = NC_NOERR;
    size_t count = nclistlength(var->maps);

    for(i=0;i<count;i++) {
        char** memory;
        char** p;
	NCD4node* group;
        /* Add an attribute to the parent variable
           listing fqn's of all specified variables in map order*/
        memory = (char**)malloc(count*sizeof(char*));
        if(memory == NULL) {ret=NC_ENOMEM; goto done;}
        p = memory;
        for(i=0;i<count;i++) {
            NCD4node* mapref = (NCD4node*)nclistget(var->maps,i);
            char* fqn = NCD4_makeFQN(mapref);
            *p++ = fqn;
        }
	group = groupFor(var);
	ret = nc_put_att(group->meta.id,var->meta.id,NC4TAGMAPS,NC_STRING,count,memory);
        freeStringMemory(memory,count);
        NCCHECK(ret);
    }
done:
    return ret;
}

static int
buildAttributes(NCD4meta* builder, NCD4node* varorgroup)
{
    int i,ret = NC_NOERR;

    for(i=0;i<nclistlength(varorgroup->attributes);i++) {
	NCD4node* attr = nclistget(varorgroup->attributes,i);
	void* memory = NULL;
	size_t count = nclistlength(attr->attr.values);
	NCD4node* group;
        int varid;

	/* Supress all UCARTAG attributes (as modified) */
	if(strncmp(attr->name,UCARTAGNC4,strlen(UCARTAGNC4)) == 0)
	    continue;

	if(ISGROUP(varorgroup->sort))
	    varid = NC_GLOBAL;
	else
	    varid = varorgroup->meta.id;
        if((ret=compileAttrValues(builder,attr->basetype,attr->attr.values,&memory))) {
	        nullfree(memory);
                FAIL(NC_ERANGE,"Malformed attribute value(s) for: %s",attr->name);
        }
	group = groupFor(varorgroup);
        NCCHECK((nc_put_att(group->meta.id,varid,attr->name,attr->basetype->meta.id,count,memory)));
        nullfree(memory);
    }
done:
    return ret;
}

static int
buildStructureType(NCD4meta* builder, NCD4node* structtype)
{
    int tid,ret = NC_NOERR;
    NCD4node* group = NULL;
    char* name = NULL;

    group = groupFor(structtype); /* default */

    /* Figure out the type name and containing group */
    if(structtype->nc4.orig.name != NULL) {
	name = strdup(structtype->nc4.orig.name);
	group = structtype->nc4.orig.group;
    } else {
        name = getFieldFQN(structtype,"_t");
    }

    /* Step 2: See if already defined */
    if(nc_inq_typeid(group->meta.id,name,&tid) == NC_NOERR) {/* Already exists */
        structtype->meta.id = tid;
        goto done;
    }    

    /* Since netcdf does not support forward references,
       we presume all field types are defined */
    if((ret=buildCompound(builder,structtype,group,name))) goto done;

done:
    nullfree(name);
    return ret;
}

static int
buildSequenceType(NCD4meta* builder, NCD4node* seqtype)
{
    int ret = NC_NOERR;
    NCD4node* group = groupFor(seqtype);
    NCD4node* ucar;
    NCD4node* field1 = NULL; /* first field */
    int usevlen = 0;
    nc_type tid = NC_NAT;
    char* vlentypename = NULL;
    char* cmpdtypename = NULL;

    /* Step 1: Figure out the type name and containing group */
    if(seqtype->nc4.orig.name != NULL) {
	vlentypename = strdup(seqtype->nc4.orig.name);
	group = seqtype->nc4.orig.group;
    } else {
        vlentypename = getFieldFQN(seqtype,"_t");
        cmpdtypename = getFieldFQN(seqtype,"_cmpd_t");
    }

    /* Step 2: See if already defined */
    if(nc_inq_typeid(group->meta.id,vlentypename,&tid) == NC_NOERR) {/* Already exists */
        seqtype->meta.id = tid;
        goto done;
    }    

    /* Step 4: determine if we need to build a structure type or can go straight to a vlen/
       Test:  UCARTAGVLEN attribute is set && there is only field */
    ucar = NCD4_findAttr(seqtype,UCARTAGVLEN);
    usevlen = (ucar != NULL && (nclistlength(seqtype->vars) == 1));

    /* Step 5: get/define the basetype of the sequence vlen */
    if(usevlen) {
	/* We use the type of the first field as the vlen type */
	NCD4node* basetype = field1->basetype;
	tid = basetype->meta.id;
    } else {
	/* we need to define a compound type */
	if((ret=buildCompound(builder,seqtype,group,cmpdtypename))) goto done;	
	/* save the compound type id */
	seqtype->meta.cmpdid = seqtype->meta.id;
	tid = seqtype->meta.cmpdid;
    }
    /* build the vlen type */
    NCCHECK(nc_def_vlen(group->meta.id, vlentypename, tid, &seqtype->meta.id));

done:
    nullfree(vlentypename);
    nullfree(cmpdtypename);
    return ret;
}

static int
buildCompound(NCD4meta* builder, NCD4node* cmpdtype, NCD4node* group, char* name)
{
    int i,ret = NC_NOERR;

    /* Step 1: compute field offsets */
    computeOffsets(builder,cmpdtype);

    /* Step 2: define this node's compound type */
    NCCHECK((nc_def_compound(group->meta.id,(size_t)cmpdtype->meta.offset,name,&cmpdtype->meta.id)));

    /* Step 3: add the fields to type */
    for(i=0;i<nclistlength(cmpdtype->vars);i++) {  
	int rank;
	int dimsizes[NC_MAX_VAR_DIMS];
        NCD4node* field = (NCD4node*)nclistget(cmpdtype->vars,i);
	rank = nclistlength(field->dims);
        if(rank == 0) { /* scalar */
            NCCHECK((nc_insert_compound(group->meta.id, cmpdtype->meta.id,
					field->name, field->meta.offset,
					field->basetype->meta.id)));
        } else if(rank > 0) { /* array  */
	    getDimsizes(field,dimsizes);
            NCCHECK((nc_insert_array_compound(group->meta.id, cmpdtype->meta.id,
					      field->name, field->meta.offset,
					      field->basetype->meta.id,
					      rank, dimsizes)));
	}
    }

done:
    return ret;
}

static int
buildAtomicVar(NCD4meta* builder, NCD4node* var)
{
    int ret = NC_NOERR;
    size_t rank;
    int dimids[NC_MAX_VAR_DIMS];
    NCD4node* group;

    group = groupFor(var);

#ifdef D4DEBUG
    fprintf(stderr,"build var: %s.%s\n",group->name,var->name); fflush(stderr);
#endif

    rank = getDimrefs(var,dimids);
    NCCHECK((nc_def_var(group->meta.id,var->name,var->basetype->meta.id,rank,dimids,&var->meta.id)));
    /* Build attributes and map attributes */
    if((ret = buildMetaData(builder,var))) goto done;    
done:
    return ret;
}

static int
buildStructure(NCD4meta* builder, NCD4node* structvar)
{
    int ret = NC_NOERR;
    NCD4node* group;
    int rank;
    int dimids[NC_MAX_VAR_DIMS];

    /* Step 1: define the variable */
    rank = nclistlength(structvar->dims);
    getDimrefs(structvar,dimids);
    group = groupFor(structvar);
    NCCHECK((nc_def_var(group->meta.id,structvar->name,structvar->basetype->meta.id,rank,dimids,&structvar->meta.id)));
    /* Build attributes and map attributes WRT the variable */
    if((ret = buildMetaData(builder,structvar))) goto done;    

done:
    return ret;
}

static int
buildSequence(NCD4meta* builder, NCD4node* seq)
{

    int ret = NC_NOERR;
    NCD4node* group;
    int rank;
    int dimids[NC_MAX_VAR_DIMS];

    rank = nclistlength(seq->dims);
    getDimrefs(seq,dimids);
    group = groupFor(seq);
    NCCHECK((nc_def_var(group->meta.id,seq->name,seq->basetype->meta.id,rank,dimids,&seq->meta.id)));
    /* Build attributes and map attributes WRT the variable */
    if((ret = buildMetaData(builder,seq))) goto done;    

done:
    return ret;
}

/***************************************************/
/* Utilities */

/* Collect FQN path from node upto (but not including)
   the first enclosing group and create an name from it
*/
static char*
getFieldFQN(NCD4node* field, const char* tail)
{
    int i;
    NCD4node* x = NULL;
    NClist* path = NULL;
    NCbytes* fqn =  NULL;
    char* result;

    path = nclistnew();
    for(x=field;!ISGROUP(x->sort);x=x->container) {
	nclistinsert(path,0,x);
    }
    fqn = ncbytesnew();
    for(i=0;i<nclistlength(path);i++) {
	NCD4node* elem = (NCD4node*)nclistget(path,i);
	char* escaped = backslashEscape(elem->name);
	if(escaped == NULL) return NULL;
	if(i > 0) ncbytesappend(fqn,'.');
	ncbytescat(fqn,escaped);
	free(escaped);
    }
    ncbytescat(fqn,tail);
    result = ncbytesextract(fqn);
    ncbytesfree(fqn);
    return result;    
}

static size_t
getDimrefs(NCD4node* var, int* dimids)
{
    int i;
    int rank = nclistlength(var->dims);
    for(i=0;i<rank;i++) {
	NCD4node* dim = (NCD4node*)nclistget(var->dims,i);
	dimids[i] = dim->meta.id;
    }
    return rank;
}

static size_t
getDimsizes(NCD4node* var, size_t* dimsizes)
{
    int i;
    int rank = nclistlength(var->dims);
    for(i=0;i<rank;i++) {
	NCD4node* dim = (NCD4node*)nclistget(var->dims,i);
	dimsizes[i] = dim->dim.size;
    }
    return rank;
}

/**************************************************/
/* Utilities */


static NCD4node*
groupFor(NCD4node* node)
{
    while(node->sort != NCD4_GROUP) node = node->container;
    return node;
}

static void
freeStringMemory(char** mem, int count)
{
    int i;
    if(mem == NULL) return;
    for(i=0;i<count;i++,mem++) {
        if(*mem) free(*mem);
    }
    free(mem);
}

/**
Convert a list of attribute value strings
into a memory chunk capable of being passed
to nc_put_att().
*/
static int
compileAttrValues(NCD4meta* builder, NCD4node* basetype, NClist* values, void** memoryp)
{
    int i,ret = NC_NOERR;
    int count = nclistlength(values);
    unsigned char* memory = NULL;
    unsigned char* p;
    size_t size;
    NCD4node* truebase = NULL;
    union ATOMICS converter;
    int isenum = 0;

    isenum = (basetype->subsort == NC_ENUM);
    truebase = (isenum ? basetype->basetype : basetype);
    if(!ISTYPE(truebase->sort) || (truebase->meta.id > NC_MAX_ATOMIC_TYPE))
        FAIL(NC_EBADTYPE,"Illegal attribute type: %s",basetype->name);
    size = NCD4_typesize(truebase->meta.id);
    if((memory = (char*)malloc(count*size))==NULL)
        return NC_ENOMEM;
    p = memory;
    for(i=0;i<count;i++) {
        char* s = (char*)nclistget(values,i);
        if(isenum) {
            if((ret=decodeEconst(builder,basetype,s,&converter)))
                FAIL(ret,"Illegal enum const: ",s);
        } else {
            if((ret = convertString(&converter,basetype,s)))
            FAIL(NC_EBADTYPE,"Illegal attribute type: ",basetype->name);
        }
        ret = downConvert(&converter,truebase);
        p = copyAtomic(&converter,truebase->meta.id,NCD4_typesize(truebase->meta.id),p);
    }
    if(memoryp) *memoryp = memory;
done:
    return ret;
}

static void*
copyAtomic(union ATOMICS* converter, nc_type type, size_t len, void* dst)
{
    switch (type) {
    case NC_CHAR: case NC_BYTE: case NC_UBYTE:
        memcpy(dst,&converter->u8[0],len); break;
    case NC_SHORT: case NC_USHORT:
        memcpy(dst,&converter->u16[0],len); break;
    case NC_INT: case NC_UINT:
        memcpy(dst,&converter->u32[0],len); break;
    case NC_INT64: case NC_UINT64:
        memcpy(dst,&converter->u64[0],len); break;
    case NC_FLOAT:
        memcpy(dst,&converter->f32[0],len); break;
    case NC_DOUBLE:
        memcpy(dst,&converter->f64[0],len); break;
    case NC_STRING:
        memcpy(dst,&converter->s[0],len); break;
    }/*switch*/
    return (((char*)dst)+len);
}

static int
convertString(union ATOMICS* converter, NCD4node* type, const char* s)
{
    switch (type->subsort) {
    case NC_BYTE:
    case NC_SHORT:
    case NC_INT:
    case NC_INT64:
	if(sscanf(s,"%lld",&converter->i64) != 1) return NC_ERANGE;
	break;
    case NC_UBYTE:
    case NC_USHORT:
    case NC_UINT:
    case NC_UINT64:
	if(sscanf(s,"%llu",&converter->u64) != 1) return NC_ERANGE;
	break;
    case NC_FLOAT:
    case NC_DOUBLE:
	if(sscanf(s,"%lf",&converter->f64) != 1) return NC_ERANGE;
	break;
    case NC_STRING:
	converter->s[0]= strdup(s);
	break;
    }/*switch*/
    return downConvert(converter,type);
}

static int
downConvert(union ATOMICS* converter, NCD4node* type)
{
    unsigned long long u64 = converter->u64[0];
    long long i64 = converter->i64[0];
    double f64 = converter->f64[0];
    char* s = converter->s[0];
    switch (type->subsort) {
    case NC_BYTE:
	converter->i8[0] = (char)i64;
	break;
    case NC_UBYTE:
	converter->u8[0] = (unsigned char)u64;
	break;
    case NC_SHORT:
	converter->i16[0] = (short)i64;
	break;
    case NC_USHORT:
	converter->u16[0] = (unsigned short)u64;
	break;
    case NC_INT:
	converter->i32[0] = (int)i64;
	break;
    case NC_UINT:
	converter->u32[0] = (unsigned int)u64;
	break;
    case NC_INT64:
	converter->i64[0] = i64;
	break;
    case NC_UINT64:
	converter->u64[0]= u64;
	break;
    case NC_FLOAT:
	converter->f32[0] = (float)f64;
	break;
    case NC_DOUBLE:
	converter->f64[0] = f64;
	break;
    case NC_STRING:
	converter->s[0]= s;
	break;
    }/*switch*/
    return NC_NOERR;
}

/*
Given an enum type, and a string representing an econst,
convert to integer.
Note: this will work if the econst string is a number or a econst name
*/
static int
decodeEconst(NCD4meta* builder, NCD4node* enumtype, const char* nameorval, union ATOMICS* converter)
{
    int i,ret=NC_NOERR;
    union ATOMICS number;
    NCD4node* match = NULL;

    /* First, see if the value is an econst name */
    for(i=0;i<nclistlength(enumtype->en.econsts);i++) {
        NCD4node* ec = (NCD4node*)nclistget(enumtype->en.econsts,i);
        if(strcmp(ec->name,nameorval)==0) {match = ec; break;}
    }
    /* If no match, try to invert as a number to see if there is a matching econst */
    if(!match) {
        /* get the incoming value as number */
        if((ret=convertString(&number,enumtype->basetype,nameorval)))
            goto done;
        for(i=0;i<nclistlength(enumtype->en.econsts);i++) {
            NCD4node* ec = (NCD4node*)nclistget(enumtype->en.econsts,i);
            if(ec->en.ecvalue.u64 == number.u64) {match = ec; break;}
        }
    }
    if(match == NULL)
        FAIL(NC_EINVAL,"No enum const matching value: %s",nameorval);
    if(converter) *converter = match->en.ecvalue;
done:
    return ret;
}

static char*
backslashEscape(const char* s)
{
    const char* p;
    char* q;
    size_t len;
    char* escaped = NULL;

    len = strlen(s);
    escaped = (char*)malloc(1+(2*len)); /* max is everychar is escaped */
    if(escaped == NULL) return NULL;
    for(p=s,q=escaped;*p;p++) {
        char c = *p;
        switch (c) {
        case '\\':
        case '/':
        case '.':
        case '@':
            *q++ = '\\'; *q++ = '\\';
            break;
        default: *q++ = c; break;
        }
    }
    *q = '\0';
    return escaped;
}

/* Compute compound type field offsets */
static void
computeOffsets(NCD4meta* builder, NCD4node* cmpd)
{
    int i;
    unsigned long long offset = 0;
    unsigned long long size = 0;

    for(i=0;i<nclistlength(cmpd->vars);i++) {
	NCD4node* field = (NCD4node*)nclistget(cmpd->vars,i);
	if(field->subsort == NC_STRUCT) {
	    computeOffsets(builder, field->basetype);
	    size = computeTypeSize(builder,field->basetype);
	} else if(field->subsort == NC_SEQ) {
	    size = sizeof(nc_vlen_t);
	} else
	    size = computeTypeSize(builder,field->basetype);
	field->meta.offset = offset;
	offset += size;
    }
    /* Save final offset as the size of the compound */
    cmpd->meta.offset = offset;
}

static unsigned long long
computeTypeSize(NCD4meta* builder, NCD4node* type)
{
    unsigned long long size = 0;

    switch (type->sort) {
    case NCD4_TYPE:
	switch (type->subsort) {
	default: size = NCD4_typesize(type->meta.id); break;
        case NC_OPAQUE:
            size = (type->opaque.size == 0 ? sizeof(nc_vlen_t) : type->opaque.size);
  	    break;
        case NC_ENUM:
   	    size = computeTypeSize(builder,type->basetype);
	    break;
        case NC_SEQ:
	    size = sizeof(nc_vlen_t);
	    break;
        case NC_STRUCT:
	    size = type->meta.offset;
#if 0
            size = 0;
            for(i=0;i<nclistlength(type->vars);i++) {
                unsigned long long count = 0;
                NCD4node* field = (NCD4node*)nclistget(type->vars,i);
                count = NCD4_dimproduct(field);
                size += (count * computeTypeSize(builder,field->basetype));
            }
#endif
        }
        break;
    default: break; /* ignore */
    }        
    return size;
}