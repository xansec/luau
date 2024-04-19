// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "Luau/Unifier2.h"

#include "Luau/Instantiation.h"
#include "Luau/Scope.h"
#include "Luau/Simplify.h"
#include "Luau/Type.h"
#include "Luau/TypeArena.h"
#include "Luau/TypeCheckLimits.h"
#include "Luau/TypeFamily.h"
#include "Luau/TypeFwd.h"
#include "Luau/TypePack.h"
#include "Luau/TypeUtils.h"
#include "Luau/VisitType.h"

#include <algorithm>
#include <optional>

LUAU_FASTINT(LuauTypeInferRecursionLimit)

namespace Luau
{

static bool areCompatible(TypeId left, TypeId right)
{
    auto p = get2<TableType, TableType>(follow(left), follow(right));
    if (!p)
        return true;

    const TableType* leftTable = p.first;
    LUAU_ASSERT(leftTable);
    const TableType* rightTable = p.second;
    LUAU_ASSERT(rightTable);

    const auto missingPropIsCompatible = [](const Property& leftProp, const TableType* rightTable) {
        // Two tables may be compatible even if their shapes aren't exactly the
        // same if the extra property is optional, free (and therefore
        // potentially optional), or if the right table has an indexer.  Or if
        // the right table is free (and therefore potentially has an indexer or
        // a compatible property)

        LUAU_ASSERT(leftProp.isReadOnly() || leftProp.isShared());

        const TypeId leftType = follow(leftProp.isReadOnly() ? *leftProp.readTy : leftProp.type());

        if (isOptional(leftType) || get<FreeType>(leftType) || rightTable->state == TableState::Free || rightTable->indexer.has_value())
            return true;

        return false;
    };

    for (const auto& [name, leftProp] : leftTable->props)
    {
        auto it = rightTable->props.find(name);
        if (it == rightTable->props.end())
        {
            if (!missingPropIsCompatible(leftProp, rightTable))
                return false;
        }
    }

    for (const auto& [name, rightProp] : rightTable->props)
    {
        auto it = leftTable->props.find(name);
        if (it == leftTable->props.end())
        {
            if (!missingPropIsCompatible(rightProp, leftTable))
                return false;
        }
    }

    return true;
}

// returns `true` if `ty` is irressolvable and should be added to `incompleteSubtypes`.
static bool isIrresolvable(TypeId ty)
{
    return get<BlockedType>(ty) || get<TypeFamilyInstanceType>(ty);
}

// returns `true` if `tp` is irressolvable and should be added to `incompleteSubtypes`.
static bool isIrresolvable(TypePackId tp)
{
    return get<BlockedTypePack>(tp) || get<TypeFamilyInstanceTypePack>(tp);
}

Unifier2::Unifier2(NotNull<TypeArena> arena, NotNull<BuiltinTypes> builtinTypes, NotNull<Scope> scope, NotNull<InternalErrorReporter> ice)
    : arena(arena)
    , builtinTypes(builtinTypes)
    , scope(scope)
    , ice(ice)
    , limits(TypeCheckLimits{}) // TODO: typecheck limits in unifier2
    , recursionLimit(FInt::LuauTypeInferRecursionLimit)
    , uninhabitedTypeFamilies(nullptr)
{
}

Unifier2::Unifier2(NotNull<TypeArena> arena, NotNull<BuiltinTypes> builtinTypes, NotNull<Scope> scope, NotNull<InternalErrorReporter> ice,
    DenseHashSet<const void*>* uninhabitedTypeFamilies)
    : arena(arena)
    , builtinTypes(builtinTypes)
    , scope(scope)
    , ice(ice)
    , limits(TypeCheckLimits{}) // TODO: typecheck limits in unifier2
    , recursionLimit(FInt::LuauTypeInferRecursionLimit)
    , uninhabitedTypeFamilies(uninhabitedTypeFamilies)
{
}

bool Unifier2::unify(TypeId subTy, TypeId superTy)
{
    subTy = follow(subTy);
    superTy = follow(superTy);

    if (auto subGen = genericSubstitutions.find(subTy))
        return unify(*subGen, superTy);

    if (auto superGen = genericSubstitutions.find(superTy))
        return unify(subTy, *superGen);

    if (seenTypePairings.contains({subTy, superTy}))
        return true;
    seenTypePairings.insert({subTy, superTy});

    if (subTy == superTy)
        return true;

    // We have potentially done some unifications while dispatching either `SubtypeConstraint` or `PackSubtypeConstraint`,
    // so rather than implementing backtracking or traversing the entire type graph multiple times, we could push
    // additional constraints as we discover blocked types along with their proper bounds.
    //
    // But we exclude these two subtyping patterns, they are tautological:
    //   - never <: *blocked*
    //   - *blocked* <: unknown
    if ((isIrresolvable(subTy) || isIrresolvable(superTy)) && !get<NeverType>(subTy) && !get<UnknownType>(superTy))
    {
        if (uninhabitedTypeFamilies && (uninhabitedTypeFamilies->contains(subTy) || uninhabitedTypeFamilies->contains(superTy)))
            return true;

        incompleteSubtypes.push_back(SubtypeConstraint{subTy, superTy});
        return true;
    }

    FreeType* subFree = getMutable<FreeType>(subTy);
    FreeType* superFree = getMutable<FreeType>(superTy);

    if (subFree && superFree)
    {
        DenseHashSet<TypeId> seen{nullptr};
        if (OccursCheckResult::Fail == occursCheck(seen, subTy, superTy))
        {
            asMutable(subTy)->ty.emplace<BoundType>(builtinTypes->errorRecoveryType());
            return false;
        }
        else if (OccursCheckResult::Fail == occursCheck(seen, superTy, subTy))
        {
            asMutable(subTy)->ty.emplace<BoundType>(builtinTypes->errorRecoveryType());
            return false;
        }

        superFree->lowerBound = mkUnion(subFree->lowerBound, superFree->lowerBound);
        superFree->upperBound = mkIntersection(subFree->upperBound, superFree->upperBound);
        asMutable(subTy)->ty.emplace<BoundType>(superTy);
    }
    else if (subFree)
    {
        return unifyFreeWithType(subTy, superTy);
    }
    else if (superFree)
    {
        superFree->lowerBound = mkUnion(superFree->lowerBound, subTy);
    }

    if (subFree || superFree)
        return true;

    if (auto subLocal = getMutable<LocalType>(subTy))
    {
        subLocal->domain = mkUnion(subLocal->domain, superTy);
        expandedFreeTypes[subTy].push_back(superTy);
    }

    auto subFn = get<FunctionType>(subTy);
    auto superFn = get<FunctionType>(superTy);
    if (subFn && superFn)
        return unify(subTy, superFn);

    auto subUnion = get<UnionType>(subTy);
    auto superUnion = get<UnionType>(superTy);
    if (subUnion)
        return unify(subUnion, superTy);
    else if (superUnion)
        return unify(subTy, superUnion);

    auto subIntersection = get<IntersectionType>(subTy);
    auto superIntersection = get<IntersectionType>(superTy);
    if (subIntersection)
        return unify(subIntersection, superTy);
    else if (superIntersection)
        return unify(subTy, superIntersection);

    auto subNever = get<NeverType>(subTy);
    auto superNever = get<NeverType>(superTy);
    if (subNever && superNever)
        return true;
    else if (subNever && superFn)
    {
        // If `never` is the subtype, then we can propagate that inward.
        bool argResult = unify(superFn->argTypes, builtinTypes->neverTypePack);
        bool retResult = unify(builtinTypes->neverTypePack, superFn->retTypes);
        return argResult && retResult;
    }
    else if (subFn && superNever)
    {
        // If `never` is the supertype, then we can propagate that inward.
        bool argResult = unify(builtinTypes->neverTypePack, subFn->argTypes);
        bool retResult = unify(subFn->retTypes, builtinTypes->neverTypePack);
        return argResult && retResult;
    }

    auto subAny = get<AnyType>(subTy);
    auto superAny = get<AnyType>(superTy);
    if (subAny && superAny)
        return true;
    else if (subAny && superFn)
    {
        // If `any` is the subtype, then we can propagate that inward.
        bool argResult = unify(superFn->argTypes, builtinTypes->anyTypePack);
        bool retResult = unify(builtinTypes->anyTypePack, superFn->retTypes);
        return argResult && retResult;
    }
    else if (subFn && superAny)
    {
        // If `any` is the supertype, then we can propagate that inward.
        bool argResult = unify(builtinTypes->anyTypePack, subFn->argTypes);
        bool retResult = unify(subFn->retTypes, builtinTypes->anyTypePack);
        return argResult && retResult;
    }

    auto subTable = getMutable<TableType>(subTy);
    auto superTable = get<TableType>(superTy);
    if (subTable && superTable)
    {
        // `boundTo` works like a bound type, and therefore we'd replace it
        // with the `boundTo` and try unification again.
        //
        // However, these pointers should have been chased already by follow().
        LUAU_ASSERT(!subTable->boundTo);
        LUAU_ASSERT(!superTable->boundTo);

        return unify(subTable, superTable);
    }

    auto subMetatable = get<MetatableType>(subTy);
    auto superMetatable = get<MetatableType>(superTy);
    if (subMetatable && superMetatable)
        return unify(subMetatable, superMetatable);
    else if (subMetatable) // if we only have one metatable, unify with the inner table
        return unify(subMetatable->table, superTy);
    else if (superMetatable) // if we only have one metatable, unify with the inner table
        return unify(subTy, superMetatable->table);

    auto [subNegation, superNegation] = get2<NegationType, NegationType>(subTy, superTy);
    if (subNegation && superNegation)
        return unify(subNegation->ty, superNegation->ty);

    // The unification failed, but we're not doing type checking.
    return true;
}

// If superTy is a function and subTy already has a
// potentially-compatible function in its upper bound, we assume that
// the function is not overloaded and attempt to combine superTy into
// subTy's existing function bound.
bool Unifier2::unifyFreeWithType(TypeId subTy, TypeId superTy)
{
    FreeType* subFree = getMutable<FreeType>(subTy);
    LUAU_ASSERT(subFree);

    auto doDefault = [&]() {
        subFree->upperBound = mkIntersection(subFree->upperBound, superTy);
        expandedFreeTypes[subTy].push_back(superTy);
        return true;
    };

    TypeId upperBound = follow(subFree->upperBound);

    if (get<FunctionType>(upperBound))
        return unify(subFree->upperBound, superTy);

    const FunctionType* superFunction = get<FunctionType>(superTy);
    if (!superFunction)
        return doDefault();

    const auto [superArgHead, superArgTail] = flatten(superFunction->argTypes);
    if (superArgTail)
        return doDefault();

    const IntersectionType* upperBoundIntersection = get<IntersectionType>(subFree->upperBound);
    if (!upperBoundIntersection)
        return doDefault();

    bool ok = true;
    bool foundOne = false;

    for (TypeId part : upperBoundIntersection->parts)
    {
        const FunctionType* ft = get<FunctionType>(follow(part));
        if (!ft)
            continue;

        const auto [subArgHead, subArgTail] = flatten(ft->argTypes);

        if (!subArgTail && subArgHead.size() == superArgHead.size())
        {
            foundOne = true;
            ok &= unify(part, superTy);
        }
    }

    if (foundOne)
        return ok;
    else
        return doDefault();
}

bool Unifier2::unify(TypeId subTy, const FunctionType* superFn)
{
    const FunctionType* subFn = get<FunctionType>(subTy);

    bool shouldInstantiate =
        (superFn->generics.empty() && !subFn->generics.empty()) || (superFn->genericPacks.empty() && !subFn->genericPacks.empty());

    if (shouldInstantiate)
    {
        for (auto generic : subFn->generics)
            genericSubstitutions[generic] = freshType(arena, builtinTypes, scope);

        for (auto genericPack : subFn->genericPacks)
            genericPackSubstitutions[genericPack] = arena->freshTypePack(scope);
    }

    bool argResult = unify(superFn->argTypes, subFn->argTypes);
    bool retResult = unify(subFn->retTypes, superFn->retTypes);
    return argResult && retResult;
}

bool Unifier2::unify(const UnionType* subUnion, TypeId superTy)
{
    bool result = true;

    // if the occurs check fails for any option, it fails overall
    for (auto subOption : subUnion->options)
    {
        if (areCompatible(subOption, superTy))
            result &= unify(subOption, superTy);
    }

    return result;
}

bool Unifier2::unify(TypeId subTy, const UnionType* superUnion)
{
    bool result = true;

    // if the occurs check fails for any option, it fails overall
    for (auto superOption : superUnion->options)
    {
        if (areCompatible(subTy, superOption))
            result &= unify(subTy, superOption);
    }

    return result;
}

bool Unifier2::unify(const IntersectionType* subIntersection, TypeId superTy)
{
    bool result = true;

    // if the occurs check fails for any part, it fails overall
    for (auto subPart : subIntersection->parts)
        result &= unify(subPart, superTy);

    return result;
}

bool Unifier2::unify(TypeId subTy, const IntersectionType* superIntersection)
{
    bool result = true;

    // if the occurs check fails for any part, it fails overall
    for (auto superPart : superIntersection->parts)
        result &= unify(subTy, superPart);

    return result;
}

bool Unifier2::unify(TableType* subTable, const TableType* superTable)
{
    bool result = true;

    // It suffices to only check one direction of properties since we'll only ever have work to do during unification
    // if the property is present in both table types.
    for (const auto& [propName, subProp] : subTable->props)
    {
        auto superPropOpt = superTable->props.find(propName);

        if (superPropOpt != superTable->props.end())
        {
            const Property& superProp = superPropOpt->second;

            if (subProp.isReadOnly() && superProp.isReadOnly())
                result &= unify(*subProp.readTy, *superPropOpt->second.readTy);
            else if (subProp.isReadOnly())
                result &= unify(*subProp.readTy, superProp.type());
            else if (superProp.isReadOnly())
                result &= unify(subProp.type(), *superProp.readTy);
            else
            {
                result &= unify(subProp.type(), superProp.type());
                result &= unify(superProp.type(), subProp.type());
            }
        }
    }

    auto subTypeParamsIter = subTable->instantiatedTypeParams.begin();
    auto superTypeParamsIter = superTable->instantiatedTypeParams.begin();

    while (subTypeParamsIter != subTable->instantiatedTypeParams.end() && superTypeParamsIter != superTable->instantiatedTypeParams.end())
    {
        result &= unify(*subTypeParamsIter, *superTypeParamsIter);

        subTypeParamsIter++;
        superTypeParamsIter++;
    }

    auto subTypePackParamsIter = subTable->instantiatedTypePackParams.begin();
    auto superTypePackParamsIter = superTable->instantiatedTypePackParams.begin();

    while (subTypePackParamsIter != subTable->instantiatedTypePackParams.end() &&
           superTypePackParamsIter != superTable->instantiatedTypePackParams.end())
    {
        result &= unify(*subTypePackParamsIter, *superTypePackParamsIter);

        subTypePackParamsIter++;
        superTypePackParamsIter++;
    }

    if (subTable->selfTy && superTable->selfTy)
        result &= unify(*subTable->selfTy, *superTable->selfTy);

    if (subTable->indexer && superTable->indexer)
    {
        result &= unify(subTable->indexer->indexType, superTable->indexer->indexType);
        result &= unify(subTable->indexer->indexResultType, superTable->indexer->indexResultType);
    }

    if (!subTable->indexer && subTable->state == TableState::Unsealed && superTable->indexer)
    {
        /*
         * Unsealed tables are always created from literal table expressions. We
         * can't be completely certain whether such a table has an indexer just
         * by the content of the expression itself, so we need to be a bit more
         * flexible here.
         *
         * If we are trying to reconcile an unsealed table with a table that has
         * an indexer, we therefore conclude that the unsealed table has the
         * same indexer.
         */
        subTable->indexer = *superTable->indexer;
    }

    return result;
}

bool Unifier2::unify(const MetatableType* subMetatable, const MetatableType* superMetatable)
{
    return unify(subMetatable->metatable, superMetatable->metatable) && unify(subMetatable->table, superMetatable->table);
}

// FIXME?  This should probably return an ErrorVec or an optional<TypeError>
// rather than a boolean to signal an occurs check failure.
bool Unifier2::unify(TypePackId subTp, TypePackId superTp)
{
    subTp = follow(subTp);
    superTp = follow(superTp);

    if (auto subGen = genericPackSubstitutions.find(subTp))
        return unify(*subGen, superTp);

    if (auto superGen = genericPackSubstitutions.find(superTp))
        return unify(subTp, *superGen);

    if (seenTypePackPairings.contains({subTp, superTp}))
        return true;
    seenTypePackPairings.insert({subTp, superTp});

    if (subTp == superTp)
        return true;

    if (isIrresolvable(subTp) || isIrresolvable(superTp))
    {
        if (uninhabitedTypeFamilies && (uninhabitedTypeFamilies->contains(subTp) || uninhabitedTypeFamilies->contains(superTp)))
            return true;

        incompleteSubtypes.push_back(PackSubtypeConstraint{subTp, superTp});
        return true;
    }

    const FreeTypePack* subFree = get<FreeTypePack>(subTp);
    const FreeTypePack* superFree = get<FreeTypePack>(superTp);

    if (subFree)
    {
        DenseHashSet<TypePackId> seen{nullptr};
        if (OccursCheckResult::Fail == occursCheck(seen, subTp, superTp))
        {
            asMutable(subTp)->ty.emplace<BoundTypePack>(builtinTypes->errorRecoveryTypePack());
            return false;
        }

        asMutable(subTp)->ty.emplace<BoundTypePack>(superTp);
        return true;
    }

    if (superFree)
    {
        DenseHashSet<TypePackId> seen{nullptr};
        if (OccursCheckResult::Fail == occursCheck(seen, superTp, subTp))
        {
            asMutable(superTp)->ty.emplace<BoundTypePack>(builtinTypes->errorRecoveryTypePack());
            return false;
        }

        asMutable(superTp)->ty.emplace<BoundTypePack>(subTp);
        return true;
    }

    size_t maxLength = std::max(flatten(subTp).first.size(), flatten(superTp).first.size());

    auto [subTypes, subTail] = extendTypePack(*arena, builtinTypes, subTp, maxLength);
    auto [superTypes, superTail] = extendTypePack(*arena, builtinTypes, superTp, maxLength);

    // right-pad the subpack with nils if `superPack` is larger since that's what a function call does
    if (subTypes.size() < maxLength)
    {
        for (size_t i = 0; i <= maxLength - subTypes.size(); i++)
            subTypes.push_back(builtinTypes->nilType);
    }

    if (subTypes.size() < maxLength || superTypes.size() < maxLength)
        return true;

    for (size_t i = 0; i < maxLength; ++i)
        unify(subTypes[i], superTypes[i]);

    if (subTail && superTail)
    {
        TypePackId followedSubTail = follow(*subTail);
        TypePackId followedSuperTail = follow(*superTail);

        if (get<FreeTypePack>(followedSubTail) || get<FreeTypePack>(followedSuperTail))
            return unify(followedSubTail, followedSuperTail);
    }
    else if (subTail)
    {
        TypePackId followedSubTail = follow(*subTail);
        if (get<FreeTypePack>(followedSubTail))
            asMutable(followedSubTail)->ty.emplace<BoundTypePack>(builtinTypes->emptyTypePack);
    }
    else if (superTail)
    {
        TypePackId followedSuperTail = follow(*superTail);
        if (get<FreeTypePack>(followedSuperTail))
            asMutable(followedSuperTail)->ty.emplace<BoundTypePack>(builtinTypes->emptyTypePack);
    }

    return true;
}

struct FreeTypeSearcher : TypeVisitor
{
    NotNull<Scope> scope;

    explicit FreeTypeSearcher(NotNull<Scope> scope)
        : TypeVisitor(/*skipBoundTypes*/ true)
        , scope(scope)
    {
    }

    enum Polarity
    {
        Positive,
        Negative,
        Both,
    };

    Polarity polarity = Positive;

    void flip()
    {
        switch (polarity)
        {
        case Positive:
            polarity = Negative;
            break;
        case Negative:
            polarity = Positive;
            break;
        case Both:
            break;
        }
    }

    // The keys in these maps are either TypeIds or TypePackIds. It's safe to
    // mix them because we only use these pointers as unique keys.  We never
    // indirect them.
    DenseHashMap<const void*, size_t> negativeTypes{0};
    DenseHashMap<const void*, size_t> positiveTypes{0};

    bool visit(TypeId ty) override
    {
        LUAU_ASSERT(ty);
        return true;
    }

    bool visit(TypeId ty, const FreeType& ft) override
    {
        if (!subsumes(scope, ft.scope))
            return true;

        switch (polarity)
        {
        case Positive:
            positiveTypes[ty]++;
            break;
        case Negative:
            negativeTypes[ty]++;
            break;
        case Both:
            positiveTypes[ty]++;
            negativeTypes[ty]++;
            break;
        }

        return true;
    }

    bool visit(TypeId ty, const TableType& tt) override
    {
        if ((tt.state == TableState::Free || tt.state == TableState::Unsealed) && subsumes(scope, tt.scope))
        {
            switch (polarity)
            {
            case Positive:
                positiveTypes[ty]++;
                break;
            case Negative:
                negativeTypes[ty]++;
                break;
            case Both:
                positiveTypes[ty]++;
                negativeTypes[ty]++;
                break;
            }
        }

        for (const auto& [_name, prop] : tt.props)
        {
            if (prop.isReadOnly())
                traverse(*prop.readTy);
            else
            {
                LUAU_ASSERT(prop.isShared());

                Polarity p = polarity;
                polarity = Both;
                traverse(prop.type());
                polarity = p;
            }
        }

        if (tt.indexer)
        {
            traverse(tt.indexer->indexType);
            traverse(tt.indexer->indexResultType);
        }

        return false;
    }

    bool visit(TypeId ty, const FunctionType& ft) override
    {
        flip();
        traverse(ft.argTypes);
        flip();

        traverse(ft.retTypes);

        return false;
    }

    bool visit(TypeId, const ClassType&) override
    {
        return false;
    }

    bool visit(TypePackId tp, const FreeTypePack& ftp) override
    {
        if (!subsumes(scope, ftp.scope))
            return true;

        switch (polarity)
        {
        case Positive:
            positiveTypes[tp]++;
            break;
        case Negative:
            negativeTypes[tp]++;
            break;
        case Both:
            positiveTypes[tp]++;
            negativeTypes[tp]++;
            break;
        }

        return true;
    }
};

struct MutatingGeneralizer : TypeOnceVisitor
{
    NotNull<BuiltinTypes> builtinTypes;

    NotNull<Scope> scope;
    DenseHashMap<const void*, size_t> positiveTypes;
    DenseHashMap<const void*, size_t> negativeTypes;
    std::vector<TypeId> generics;
    std::vector<TypePackId> genericPacks;

    bool isWithinFunction = false;

    MutatingGeneralizer(NotNull<BuiltinTypes> builtinTypes, NotNull<Scope> scope, DenseHashMap<const void*, size_t> positiveTypes,
        DenseHashMap<const void*, size_t> negativeTypes)
        : TypeOnceVisitor(/* skipBoundTypes */ true)
        , builtinTypes(builtinTypes)
        , scope(scope)
        , positiveTypes(std::move(positiveTypes))
        , negativeTypes(std::move(negativeTypes))
    {
    }

    static void replace(DenseHashSet<TypeId>& seen, TypeId haystack, TypeId needle, TypeId replacement)
    {
        haystack = follow(haystack);

        if (seen.find(haystack))
            return;
        seen.insert(haystack);

        std::vector<TypeId>* parts = nullptr;
        if (UnionType* ut = getMutable<UnionType>(haystack))
            parts = &ut->options;
        else if (IntersectionType* it = getMutable<IntersectionType>(needle))
            parts = &it->parts;
        else
            return;

        LUAU_ASSERT(parts);

        for (TypeId& option : *parts)
        {
            // FIXME: I bet this function has reentrancy problems
            option = follow(option);
            if (option == needle)
                option = replacement;

            // TODO seen set
            else if (get<UnionType>(option))
                replace(seen, option, needle, haystack);
            else if (get<IntersectionType>(option))
                replace(seen, option, needle, haystack);
        }
    }

    bool visit(TypeId ty, const FunctionType& ft) override
    {
        const bool oldValue = isWithinFunction;

        isWithinFunction = true;

        traverse(ft.argTypes);
        traverse(ft.retTypes);

        isWithinFunction = oldValue;

        return false;
    }

    bool visit(TypeId ty, const FreeType&) override
    {
        const FreeType* ft = get<FreeType>(ty);
        LUAU_ASSERT(ft);

        traverse(ft->lowerBound);
        traverse(ft->upperBound);

        // It is possible for the above traverse() calls to cause ty to be
        // transmuted.  We must reaquire ft if this happens.
        ty = follow(ty);
        ft = get<FreeType>(ty);
        if (!ft)
            return false;

        const size_t positiveCount = getCount(positiveTypes, ty);
        const size_t negativeCount = getCount(negativeTypes, ty);

        if (!positiveCount && !negativeCount)
            return false;

        const bool hasLowerBound = !get<NeverType>(follow(ft->lowerBound));
        const bool hasUpperBound = !get<UnknownType>(follow(ft->upperBound));

        DenseHashSet<TypeId> seen{nullptr};
        seen.insert(ty);

        if (!hasLowerBound && !hasUpperBound)
        {
            if (!isWithinFunction || (positiveCount + negativeCount == 1))
                emplaceType<BoundType>(asMutable(ty), builtinTypes->unknownType);
            else
            {
                emplaceType<GenericType>(asMutable(ty), scope);
                generics.push_back(ty);
            }
        }

        // It is possible that this free type has other free types in its upper
        // or lower bounds.  If this is the case, we must replace those
        // references with never (for the lower bound) or unknown (for the upper
        // bound).
        //
        // If we do not do this, we get tautological bounds like a <: a <: unknown.
        else if (positiveCount && !hasUpperBound)
        {
            TypeId lb = follow(ft->lowerBound);
            if (FreeType* lowerFree = getMutable<FreeType>(lb); lowerFree && lowerFree->upperBound == ty)
                lowerFree->upperBound = builtinTypes->unknownType;
            else
            {
                DenseHashSet<TypeId> replaceSeen{nullptr};
                replace(replaceSeen, lb, ty, builtinTypes->unknownType);
            }
            emplaceType<BoundType>(asMutable(ty), lb);
        }
        else
        {
            TypeId ub = follow(ft->upperBound);
            if (FreeType* upperFree = getMutable<FreeType>(ub); upperFree && upperFree->lowerBound == ty)
                upperFree->lowerBound = builtinTypes->neverType;
            else
            {
                DenseHashSet<TypeId> replaceSeen{nullptr};
                replace(replaceSeen, ub, ty, builtinTypes->neverType);
            }
            emplaceType<BoundType>(asMutable(ty), ub);
        }

        return false;
    }

    size_t getCount(const DenseHashMap<const void*, size_t>& map, const void* ty)
    {
        if (const size_t* count = map.find(ty))
            return *count;
        else
            return 0;
    }

    bool visit(TypeId ty, const TableType&) override
    {
        const size_t positiveCount = getCount(positiveTypes, ty);
        const size_t negativeCount = getCount(negativeTypes, ty);

        // FIXME: Free tables should probably just be replaced by upper bounds on free types.
        //
        // eg never <: 'a <: {x: number} & {z: boolean}

        if (!positiveCount && !negativeCount)
            return true;

        TableType* tt = getMutable<TableType>(ty);
        LUAU_ASSERT(tt);

        tt->state = TableState::Sealed;

        return true;
    }

    bool visit(TypePackId tp, const FreeTypePack& ftp) override
    {
        if (!subsumes(scope, ftp.scope))
            return true;

        tp = follow(tp);

        const size_t positiveCount = getCount(positiveTypes, tp);
        const size_t negativeCount = getCount(negativeTypes, tp);

        if (1 == positiveCount + negativeCount)
            asMutable(tp)->ty.emplace<BoundTypePack>(builtinTypes->unknownTypePack);
        else
        {
            asMutable(tp)->ty.emplace<GenericTypePack>(scope);
            genericPacks.push_back(tp);
        }

        return true;
    }
};

std::optional<TypeId> Unifier2::generalize(TypeId ty)
{
    ty = follow(ty);

    if (ty->owningArena != arena || ty->persistent)
        return ty;

    if (const FunctionType* ft = get<FunctionType>(ty); ft && (!ft->generics.empty() || !ft->genericPacks.empty()))
        return ty;

    FreeTypeSearcher fts{scope};
    fts.traverse(ty);

    MutatingGeneralizer gen{builtinTypes, scope, std::move(fts.positiveTypes), std::move(fts.negativeTypes)};

    gen.traverse(ty);

    /* MutatingGeneralizer mutates types in place, so it is possible that ty has
     * been transmuted to a BoundType. We must follow it again and verify that
     * we are allowed to mutate it before we attach generics to it.
     */
    ty = follow(ty);

    if (ty->owningArena != arena || ty->persistent)
        return ty;

    FunctionType* ftv = getMutable<FunctionType>(ty);
    if (ftv)
    {
        ftv->generics = std::move(gen.generics);
        ftv->genericPacks = std::move(gen.genericPacks);
    }

    return ty;
}

TypeId Unifier2::mkUnion(TypeId left, TypeId right)
{
    left = follow(left);
    right = follow(right);

    return simplifyUnion(builtinTypes, arena, left, right).result;
}

TypeId Unifier2::mkIntersection(TypeId left, TypeId right)
{
    left = follow(left);
    right = follow(right);

    return simplifyIntersection(builtinTypes, arena, left, right).result;
}

OccursCheckResult Unifier2::occursCheck(DenseHashSet<TypeId>& seen, TypeId needle, TypeId haystack)
{
    RecursionLimiter _ra(&recursionCount, recursionLimit);

    OccursCheckResult occurrence = OccursCheckResult::Pass;

    auto check = [&](TypeId ty) {
        if (occursCheck(seen, needle, ty) == OccursCheckResult::Fail)
            occurrence = OccursCheckResult::Fail;
    };

    needle = follow(needle);
    haystack = follow(haystack);

    if (seen.find(haystack))
        return OccursCheckResult::Pass;

    seen.insert(haystack);

    if (get<ErrorType>(needle))
        return OccursCheckResult::Pass;

    if (!get<FreeType>(needle))
        ice->ice("Expected needle to be free");

    if (needle == haystack)
        return OccursCheckResult::Fail;

    if (auto haystackFree = get<FreeType>(haystack))
    {
        check(haystackFree->lowerBound);
        check(haystackFree->upperBound);
    }
    else if (auto ut = get<UnionType>(haystack))
    {
        for (TypeId ty : ut->options)
            check(ty);
    }
    else if (auto it = get<IntersectionType>(haystack))
    {
        for (TypeId ty : it->parts)
            check(ty);
    }

    return occurrence;
}

OccursCheckResult Unifier2::occursCheck(DenseHashSet<TypePackId>& seen, TypePackId needle, TypePackId haystack)
{
    needle = follow(needle);
    haystack = follow(haystack);

    if (seen.find(haystack))
        return OccursCheckResult::Pass;

    seen.insert(haystack);

    if (getMutable<ErrorTypePack>(needle))
        return OccursCheckResult::Pass;

    if (!getMutable<FreeTypePack>(needle))
        ice->ice("Expected needle pack to be free");

    RecursionLimiter _ra(&recursionCount, recursionLimit);

    while (!getMutable<Unifiable::Error>(haystack))
    {
        if (needle == haystack)
            return OccursCheckResult::Fail;

        if (auto a = get<TypePack>(haystack); a && a->tail)
        {
            haystack = follow(*a->tail);
            continue;
        }

        break;
    }

    return OccursCheckResult::Pass;
}

} // namespace Luau
