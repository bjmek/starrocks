// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

package com.starrocks.sql.optimizer.rule.transformation;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.Utils;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalAggregationOperator;
import com.starrocks.sql.optimizer.operator.logical.LogicalApplyOperator;
import com.starrocks.sql.optimizer.operator.logical.LogicalFilterOperator;
import com.starrocks.sql.optimizer.operator.logical.LogicalProjectOperator;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rule.RuleType;

import java.util.List;
import java.util.Map;

// Pattern:
//      ApplyNode
//      /      \
//  LEFT     Aggregate(scalar aggregation)
//               \
//               Project
//                 \
//                 Filter
//                   \
//                   ....
//
// Before:
//      ApplyNode
//      /      \
//  LEFT     Aggregate(scalar aggregation)
//               \
//               Project
//                 \
//                 Filter
//                   \
//                   ....
// After:
//      ApplyNode
//      /      \
//  LEFT     Aggregate(scalar aggregation)
//               \
//               Filter
//                 \
//                 Project
//                   \
//                   ....
//
public class PushDownApplyAggProjectFilterRule extends TransformationRule {
    public PushDownApplyAggProjectFilterRule() {
        super(RuleType.TF_PUSH_DOWN_APPLY_AGG, Pattern.create(OperatorType.LOGICAL_APPLY).addChildren(
                Pattern.create(OperatorType.PATTERN_LEAF),
                Pattern.create(OperatorType.LOGICAL_AGGR).addChildren(
                        Pattern.create(OperatorType.LOGICAL_PROJECT).addChildren(
                                Pattern.create(OperatorType.LOGICAL_FILTER, OperatorType.PATTERN_LEAF)))));
    }

    @Override
    public boolean check(OptExpression input, OptimizerContext context) {
        // must be correlation subquery
        if (!Utils.containsCorrelationSubquery(input.getGroupExpression())) {
            return false;
        }

        LogicalAggregationOperator aggregate = (LogicalAggregationOperator) input.getInputs().get(1).getOp();

        // Don't support
        // 1. More grouping column and not Exists subquery
        // 2. Distinct aggregation(same with group by xxx)
        return aggregate.getGroupingKeys().isEmpty() || ((LogicalApplyOperator) input.getOp()).isExistential();
    }

    @Override
    public List<OptExpression> transform(OptExpression input, OptimizerContext context) {
        OptExpression aggOptExpression = input.getInputs().get(1);
        OptExpression projectExpression = aggOptExpression.getInputs().get(0);
        OptExpression filterExpression = projectExpression.getInputs().get(0);

        LogicalApplyOperator apply = (LogicalApplyOperator) input.getOp();
        LogicalAggregationOperator aggregate = (LogicalAggregationOperator) aggOptExpression.getOp();
        LogicalProjectOperator lpo = (LogicalProjectOperator) projectExpression.getOp();
        LogicalFilterOperator lfo = (LogicalFilterOperator) filterExpression.getOp();

        List<ColumnRefOperator> filterInput = Utils.extractColumnRef(lfo.getPredicate());
        filterInput.removeAll(apply.getCorrelationColumnRefs());

        Map<ColumnRefOperator, ScalarOperator> newProjectMap = Maps.newHashMap();
        newProjectMap.putAll(lpo.getColumnRefMap());
        filterInput.forEach(d -> newProjectMap.putIfAbsent(d, d));

        projectExpression.getInputs().clear();
        projectExpression.getInputs().addAll(filterExpression.getInputs());

        OptExpression newProjectOptExpression =
                OptExpression.create(new LogicalProjectOperator(newProjectMap), filterExpression.getInputs());

        OptExpression newFilterOptExpression =
                OptExpression.create(new LogicalFilterOperator(lfo.getPredicate()), newProjectOptExpression);

        OptExpression newAggOptExpression = OptExpression
                .create(new LogicalAggregationOperator(aggregate.getType(), aggregate.getGroupingKeys(),
                        aggregate.getAggregations()), newFilterOptExpression);

        OptExpression newApplyOptExpression = OptExpression
                .create(new LogicalApplyOperator(apply.getOutput(), apply.getSubqueryOperator(),
                                apply.getCorrelationColumnRefs(), apply.getCorrelationConjuncts(), apply.getPredicate(),
                                apply.isNeedCheckMaxRows(), apply.isFromAndScope()),
                        input.getInputs().get(0), newAggOptExpression);

        return Lists.newArrayList(newApplyOptExpression);
    }
}
